#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/solver.h"
#include "lib/error.h"
#include "lib/timer.h"

#include "backends/p4tools/common/control_plane/symbolic_variables.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

struct PhaseConditions {
    int inputPort = -1;
    int outputPort = -1;
    // tableName → keyName → concrete key value (Exact match only)
    std::map<cstring, std::map<cstring, const IR::Constant *>> tableKeyMap;

    bool operator==(const PhaseConditions &other) const {
        if (inputPort != other.inputPort || outputPort != other.outputPort) return false;
        if (tableKeyMap.size() != other.tableKeyMap.size()) return false;
        for (const auto &[tblName, keyMap] : tableKeyMap) {
            // TODO: check validity of table rules
            auto it = other.tableKeyMap.find(tblName);
            if (it == other.tableKeyMap.end()) return false;
            const auto &otherKeyMap = it->second;
            if (keyMap.size() != otherKeyMap.size()) return false;
            for (const auto &[keyName, concreteVal] : keyMap) {
                auto kit = otherKeyMap.find(keyName);
                if (kit == otherKeyMap.end()) return false;
                if (concreteVal->value != kit->second->value) return false;
            }
        }
        return true;
    }
};

// Build a PhaseConditions from a terminal FinalState, extracting concrete port values and
// all Exact-match table key concrete values from the evaluated tableconfigs test objects.
static PhaseConditions buildPhaseCondition(const FinalState &fs, const ProgramInfo &programInfo) {
    PhaseConditions cond;
    const auto &model = fs.getFinalModel();
    const auto *es = fs.getExecutionState();
    cond.inputPort  = IR::getIntFromLiteral(
        model.evaluate(es->get(programInfo.getTargetInputPortVar()),  true));
    cond.outputPort = IR::getIntFromLiteral(
        model.evaluate(es->get(programInfo.getTargetOutputPortVar()), true));
    for (const auto &[tblName, tblObj] : es->getTestObjectCategory("tableconfigs"_cs)) {
        const auto *evaluated = tblObj->evaluate(model, /*doComplete=*/true);
        const auto *cfg = evaluated->to<TableConfig>();
        if (cfg == nullptr) continue;
        for (const auto &rule : *cfg->getRules()) {
            for (const auto &[keyName, match] : *rule.getMatches()) {
                const auto *exact = match->to<Exact>();
                if (exact == nullptr) continue;
                cond.tableKeyMap[tblName][keyName] = exact->getEvaluatedValue();
            }
        }
    }
    return cond;
}

StateDependencyTracker::StateDependencyTracker(
    AbstractSolver &solver, const ProgramInfo &programInfo,
    const P4StateDependency::StateDependencyResult &sdResult, StateDependencyPolicy policy)
    : SymbolicExecutor(solver, programInfo), sdResult(sdResult), policy(policy) {}

std::map<cstring, std::vector<const P4StateDependency::DependencyGraphs::SOChain *>>
StateDependencyTracker::collectChains() const {
    std::map<cstring, std::vector<const P4StateDependency::DependencyGraphs::SOChain *>> result;

    auto addChains = [&](const std::map<cstring, std::vector<P4StateDependency::DependencyGraphs::SOChain>> &chainMap,
                        cstring chainName = ""_cs) {
        for (const auto &[graphName, chains] : chainMap)
            for (const auto &chain : chains)
                result[chainName].push_back(&chain);
    };

    switch (policy) {
        case StateDependencyPolicy::Tampering:
            addChains(sdResult.dataWriteValueChains, "Write Value"_cs);
            addChains(sdResult.dataWriteCondChains, "Write Condition"_cs);
            break;
        case StateDependencyPolicy::AlteringPath:
            addChains(sdResult.dataWriteCondChains, "Write Condition"_cs);
            break;
    }
    return result;
}

P4::Coverage::CoverageSet StateDependencyTracker::buildRequiredNodes(
    const P4StateDependency::DependencyGraphs::SOChain &chain) const {
    P4::Coverage::CoverageSet nodes;
    switch (policy) {
        case StateDependencyPolicy::Tampering:
            // Phase 2 targets write nodes; Phase 1 and Phase 3 target read nodes.
            // TODO: Store if and else for IfStatement
            if (currentPhase == TamperingPhase::Phase2_Write) {
                for (const auto &[v, node] : chain.writeNodes)
                    if (node != nullptr) nodes.insert(node);
            } else {
                for (const auto &[v, node] : chain.readNodes)
                    if (node != nullptr) nodes.insert(node);
            }
            break;
        case StateDependencyPolicy::AlteringPath:
            for (const auto &[v, node] : chain.writeNodes)
                if (node != nullptr) nodes.insert(node);
            for (const auto &[v, node] : chain.readNodes)
                if (node != nullptr) nodes.insert(node);
            break;
    }
    return nodes;
}

// ---------------------------------------------------------------------------
// EXIT-leaf detection
// ---------------------------------------------------------------------------

bool StateDependencyTracker::chainHasExitLeaf(
    const P4StateDependency::DependencyGraphs::SOChain &chain) {
    // readNodes only contains entries with non-null IR nodes (EXIT vertices are skipped
    // during SOChain construction because their ESG node pointer is nullptr).
    // If readVertices is larger, some vertices were EXIT/ENTRY nodes.
    return chain.readVertices.size() > chain.readNodes.size();
}

// ---------------------------------------------------------------------------
// Three-phase tampering entry point
// ---------------------------------------------------------------------------

void StateDependencyTracker::runTampering(const TamperingCallback &callBack) {
    auto chains = collectChains();
    if (chains.empty()) {
        warning("State-dependency analysis produced no chains for the selected policy.");
        return;
    }
    auto &initState = ExecutionState::create(&programInfo.getP4Program());
    runTamperingScenario(callBack, initState);
}

// RAII helper — saves/restores SymbexOptions fields around a phase run
struct ScopedSymbexOpts {
    bool savedOutputPacketOnly;
    bool savedDistinctIOPorts;
    ScopedSymbexOpts(bool setOutputPacketOnly) {
        auto &opts = SymbexOptions::get();
        savedOutputPacketOnly = opts.outputPacketOnly;
        savedDistinctIOPorts  = opts.distinctIOPorts;
        opts.outputPacketOnly = setOutputPacketOnly;
        opts.distinctIOPorts  = true;
    }
    ~ScopedSymbexOpts() {
        auto &opts = SymbexOptions::get();
        opts.outputPacketOnly = savedOutputPacketOnly;
        opts.distinctIOPorts  = savedDistinctIOPorts;
    }
};

void StateDependencyTracker::runTamperingScenario(const TamperingCallback &callBack,
                                                   const ExecutionState &initState) {
    // Helper: extract concrete (input, output) port pair from a final state
    auto getPortPair = [&](const FinalState *fs) -> std::pair<int, int> {
        const auto &model = fs->getFinalModel();
        const auto *es = fs->getExecutionState();
        int ip = IR::getIntFromLiteral(model.evaluate(es->get(programInfo.getTargetInputPortVar()),  true));
        int op = IR::getIntFromLiteral(model.evaluate(es->get(programInfo.getTargetOutputPortVar()), true));
        return {ip, op};
    };

    auto chains = collectChains();

    for (const auto &[chainName, chainList] : chains) {
        for (auto it = chainList.rbegin(); it != chainList.rend(); ++it) {
            const auto *chain = *it;
            currentChain = chain;
            currentChainName = chainName;
            printInfo("============ Chain (%1%) id=%2% %3% [Tampering 3-phase] ============",
                      chainName, chain->id, chain->soName);

            bool hasExit = chainHasExitLeaf(*chain);

            // Reset the incremental Z3 solver state between chains.  After the previous
            // chain's Phase-2 DFS + processPhase() callback, p4Assertions contains that
            // chain's complex write-path constraints (NEQ port/table conditions plus concolic
            // assignments).  If left in place, Z3's accumulated heuristics (VSIDS scores,
            // learned clauses) from the write-path exploration degrade solver performance for
            // this chain's Phase-1 read-path queries, causing branches to be incorrectly
            // pruned as unsatisfiable and leaving the DFS with no terminal state.
            // checkSat({}) pops all outstanding assertions, resetting p4Assertions /
            // checkpoints / declaredVarsById to empty so Phase 1 starts from a clean slate.
            solver.checkSat({});

            // ---- Phase 1: read original register value ----
            currentPhase = TamperingPhase::Phase1_Read;
            currentRequiredNodes = buildRequiredNodes(*chain);
            if (currentRequiredNodes.empty()) {
                warning("[Tampering] Chain id=%1% has no readNodes; skipping.", chain->id);
                continue;
            }
            printInfo("[Tampering] Phase 1 (Read) — %1% required nodes", currentRequiredNodes.size());
            for (const auto *node : currentRequiredNodes)
                printInfo("  [%1%] %2% %3%", node->node_type_name(), node,
                          node->getSourceInfo().toPositionString());

            std::vector<const FinalState *> phase1States;
            {
                ScopedSymbexOpts guard(hasExit);
                auto &phase1Init = initState.clone();
                runPhase(phase1Init, phase1States);
            }
            if (phase1States.empty()) {
                warning("[Tampering] Phase 1 found no terminal state for chain id=%1%.", chain->id);
                continue;
            }

            // Build deduplicated PhaseConditions (port pair + table key values) for Phase 1.
            std::vector<PhaseConditions> phase1Conditions;
            std::map<size_t, size_t> phase1StateToCondition;
            const IR::Expression *inputPortSymExpr = nullptr;

            for (size_t i = 0; i < phase1States.size(); ++i) {
                const auto *fs1 = phase1States[i];
                inputPortSymExpr = fs1->getExecutionState()->get(programInfo.getTargetInputPortVar());
                auto cond = buildPhaseCondition(*fs1, programInfo);
                BUG_CHECK(cond.inputPort >= 0,  "Phase 1 invalid input port %1%",  cond.inputPort);
                BUG_CHECK(cond.outputPort >= 0, "Phase 1 invalid output port %1%", cond.outputPort);
                BUG_CHECK(cond.inputPort != cond.outputPort,
                          "Phase 1 identical input/output ports %1%", cond.inputPort);

                auto it = std::find(phase1Conditions.begin(), phase1Conditions.end(), cond);
                size_t idx;
                if (it == phase1Conditions.end()) {
                    idx = phase1Conditions.size();
                    phase1Conditions.push_back(cond);
                    printInfo("[Tampering] Phase 1 chose input_port=%1% output_port=%2%",
                              cond.inputPort, cond.outputPort);
                } else {
                    idx = static_cast<size_t>(std::distance(phase1Conditions.begin(), it));
                }
                phase1StateToCondition[i] = idx;
            }

            // ---- Phase 2: write tampered value ----
            currentPhase = TamperingPhase::Phase2_Write;
            currentRequiredNodes = buildRequiredNodes(*chain);
            if (currentRequiredNodes.empty()) {
                warning("[Tampering] Chain id=%1% has no writeNodes; skipping.", chain->id);
                continue;
            }
            printInfo("[Tampering] Phase 2 (Write) — %1% required nodes", currentRequiredNodes.size());
            for (const auto *node : currentRequiredNodes)
                printInfo("  [%1%] %2% %3%", node->node_type_name(), node,
                          node->getSourceInfo().toPositionString());

            // Store phase2States per phase1 condition bucket
            std::map<size_t, std::vector<const FinalState *>> phase2StateMap;
            size_t phase2StateNum = 0;

            for (size_t i = 0; i < phase1Conditions.size(); ++i) {
                const auto &cond1 = phase1Conditions[i];
                ScopedSymbexOpts guard(/*outputPacketOnly=*/false);
                auto &phase2Init = initState.clone();
                // Constrain Phase 2's input port to differ from Phase 1's input AND output
                phase2Init.pushPathConstraint(
                    new IR::Neq(inputPortSymExpr,
                                IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
                phase2Init.pushPathConstraint(
                    new IR::Neq(inputPortSymExpr,
                                IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
                // Constrain Phase 2's table match keys to differ from Phase 1's, so the two
                // phases produce compatible (non-conflicting) table entries that can coexist.
                for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
                    for (const auto &[keyName, concreteVal] : keyMap) {
                        const auto *ctrlPlaneKey =
                            ControlPlaneState::getTableKey(tblName, keyName, concreteVal->type);
                        phase2Init.pushPathConstraint(new IR::Neq(ctrlPlaneKey, concreteVal));
                    }
                }
                runPhase(phase2Init, phase2StateMap[i]);
                phase2StateNum += phase2StateMap[i].size();
            }
            if (phase2StateNum == 0) {
                warning("[Tampering] Phase 2 found no terminal state for chain id=%1%.", chain->id);
                continue;
            }

            // Log Phase 2 port pairs (deduplicated per Phase-1 condition bucket)
            for (size_t i = 0; i < phase1Conditions.size(); ++i) {
                const auto &cond1 = phase1Conditions[i];
                for (const auto *fs2 : phase2StateMap[i]) {
                    auto portPair = getPortPair(fs2);
                    printInfo("[Tampering] Phase 2 chose input_port=%1% output_port=%2% from Phase 1 ports %3%/%4%",
                                portPair.first, portPair.second, cond1.inputPort, cond1.outputPort);
                }
            }

            // ---- Phase 3: dynamic (no symbex — test script replays Phase 1's packet) ----
            for (size_t i = 0; i < phase1States.size(); ++i) {
                const auto *fs1 = phase1States[i];
                const auto &cond1 = phase1Conditions[phase1StateToCondition[i]];

                for (const auto *fs2 : phase2StateMap[phase1StateToCondition[i]]) {
                    // Derive attacker-chosen register values from Phase 2.
                    // withAttackerValues() is called on the *unevaluated* register object so
                    // that symbolic write expressions are still available to build constraints.
                    // It returns (a) the register seeded with concrete attacker-chosen values
                    // and (b) model overrides applied to processPhase(phase2) so the emitted
                    // Phase 2 input packet shows the attacker-chosen value.
                    std::map<cstring, const TestObject *> attackerRegValues;
                    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>>
                        phase2ModelOverrides;
                    for (const auto &[regName, regObj] :
                             fs2->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
                        if (regName != chain->soName) continue;
                        auto [attackerValue, overrides] =
                            regObj->withAttackerValues(fs2->getFinalModel(),
                                                       SymbexOptions::get().stateTamperValue);
                        attackerRegValues[regName] = attackerValue;
                        phase2ModelOverrides.insert(phase2ModelOverrides.end(),
                                                    overrides.begin(), overrides.end());
                        for (const auto &[symVar, val] : overrides) {
                            printInfo("[Tampering] Phase 2 register override: "
                                      "register='%1%' symVar='%2%' value=0x%3%",
                                      regName, symVar->label,
                                      val->value.str(0, std::ios_base::hex));
                        }
                        if (overrides.empty()) {
                            printInfo("[Tampering] Phase 3 register '%1%': "
                                      "no symbolic var found — value not injectable into Phase 2 packet",
                                      regName);
                        }
                    }
                    if (attackerRegValues.empty()) {
                        warning("[Tampering] No register matching '%1%' found in Phase 2 state "
                                "for chain id=%2%; skipping.", chain->soName, chain->id);
                        continue;
                    }
                    auto [ip2, op2] = getPortPair(fs2);
                    // Phase 3 is purely dynamic: the test script replays Phase 1's packet after
                    // Phase 2 writes the attacker-chosen value to the register.
                    TamperingFinalState ts{*fs1, *fs2, hasExit,
                                           cond1.inputPort, cond1.outputPort,
                                           ip2, op2,
                                           attackerRegValues, phase2ModelOverrides};
                    if (callBack(ts)) return;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Single-phase DFS helper
// ---------------------------------------------------------------------------

void StateDependencyTracker::runPhase(ExecutionState &phaseInit,
                                       std::vector<const FinalState *> &out) {
    unexploredBranches.clear();
    // phaseInit is a caller-owned clone. The caller is responsible for pushing any
    // Z3 path constraints (e.g., port equality/exclusion from Phase 1's symbolic
    // variable) before calling this function.
    runImpl([&out, this](const FinalState &fs) -> bool {
        const auto *es = fs.getExecutionState();
        const auto &opts = SymbexOptions::get();

        if (opts.outputPacketOnly &&
            (es->getPacketBufferSize() <= 0 || es->getProperty<bool>("drop"_cs))) {
            return false;
        }

        // distinctIOPorts: always a terminal-state check because the output port is
        // assigned during execution (not a free initial symbolic variable).
        if (opts.distinctIOPorts) {
            const auto &model = fs.getFinalModel();
            const auto *ipVal =
                model.evaluate(es->get(programInfo.getTargetInputPortVar()), true);
            auto inputPort = IR::getIntFromLiteral(ipVal);
            const auto *opVal =
                model.evaluate(es->get(programInfo.getTargetOutputPortVar()), true);
            if (inputPort == IR::getIntFromLiteral(opVal)) {
                return false;
            }
        }

        out.push_back(new FinalState(fs));
        return out.size() >= 1;
    }, phaseInit);
}

// ---------------------------------------------------------------------------
// Guided DFS
// ---------------------------------------------------------------------------

std::optional<ExecutionStateReference> StateDependencyTracker::pickSuccessor(
    StepResult successors) {
    if (successors->empty()) return std::nullopt;
    if (successors->size() == 1) return successors->at(0).nextState;

    // Find the first branch that intersects currentRequiredNodes via potentialNodes
    // or already-visited nodes.
    for (size_t i = 0; i < successors->size(); ++i) {
        auto &branch = successors->at(i);
        bool hits = false;

        for (const auto *node : branch.potentialNodes) {
            if (currentRequiredNodes.count(node) != 0U) {
                hits = true;
                break;
            }
        }
        if (!hits) {
            for (const auto *node : branch.nextState.get().getVisited()) {
                if (currentRequiredNodes.count(node) != 0U) {
                    hits = true;
                    break;
                }
            }
        }

        if (hits) {
            auto chosen = branch;
            successors->erase(successors->begin() + static_cast<ptrdiff_t>(i));
            unexploredBranches.insert(unexploredBranches.end(), successors->begin(),
                                      successors->end());
            return chosen.nextState;
        }
    }

    // No branch covers required nodes; pick at random and keep the rest.
    auto chosen = popRandomBranch(*successors);
    unexploredBranches.insert(unexploredBranches.end(), successors->begin(), successors->end());
    return chosen.nextState;
}

void StateDependencyTracker::runImpl(const Callback &callBack,
                                     ExecutionStateReference executionState) {
    while (true) {
        try {
            if (executionState.get().isTerminal()) {
                // Only emit a test when the path covers all required nodes.
                const auto &visited = executionState.get().getVisited();
                bool allCovered =
                    std::all_of(currentRequiredNodes.begin(), currentRequiredNodes.end(),
                                [&visited](const IR::Node *n) { return visited.count(n) > 0; });
                if (allCovered) {
                    printInfo("[SDTrack] Test found: chain=%1% id=%2% SO=%3% (%4% nodes)",
                                currentChainName, currentChain->id, currentChain->soName,
                                currentRequiredNodes.size());
                    for (const auto *node : currentRequiredNodes) {
                        printInfo("  covered: [%1%] %2% %3%",
                                    node->node_type_name(), node,
                                    node->getSourceInfo().toPositionString());
                    }
                    bool terminate = handleTerminalState(callBack, executionState);
                    if (terminate) return;
                }
            } else {
                StepResult successors = step(executionState);
                auto next = pickSuccessor(successors);
                if (next.has_value()) {
                    executionState = next.value();
                    continue;
                }
            }
        } catch (SymbexUnimplemented &e) {
            if (SymbexOptions::get().strict) throw;
            warning("Path encountered unimplemented feature. Message: %1%\n", e.what());
        }

        // Backtrack (LIFO).
        if (unexploredBranches.empty()) return;
        Util::ScopedTimer chooseBranchTimer("branch_selection");
        executionState = unexploredBranches.back().nextState;
        unexploredBranches.pop_back();
    }
}

}  // namespace P4::P4Tools::Symbex
