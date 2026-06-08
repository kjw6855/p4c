#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"

#include <algorithm>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/solver.h"
#include "lib/error.h"
#include "lib/timer.h"

#include "backends/p4tools/common/control_plane/symbolic_variables.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/variables.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/table_stepper.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

struct PhaseConditions {
    int inputPort = -1;
    int outputPort = -1;
    // tableName → keyName → concrete evaluated match (all match kinds)
    std::map<cstring, std::map<cstring, const TableMatch *>> tableKeyMap;

    bool operator==(const PhaseConditions &other) const {
        if (inputPort != other.inputPort || outputPort != other.outputPort) return false;
        if (tableKeyMap.size() != other.tableKeyMap.size()) return false;
        for (const auto &[tblName, keyMap] : tableKeyMap) {
            auto it = other.tableKeyMap.find(tblName);
            if (it == other.tableKeyMap.end()) return false;
            if (keyMap.size() != it->second.size()) return false;
            for (const auto &[keyName, match] : keyMap) {
                auto kit = it->second.find(keyName);
                if (kit == it->second.end()) return false;
                if (!match->isEqualTo(kit->second)) return false;
            }
        }
        return true;
    }
};

// Build a PhaseConditions from a terminal FinalState, extracting concrete port values and
// all table key concrete matches (any match kind) from the evaluated tableconfigs test objects.
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
                cond.tableKeyMap[tblName][keyName] = match;
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
            addChains(sdResult.dataWriteKeyChains, "Write Key"_cs);
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
// Key → Table mapping helpers
// ---------------------------------------------------------------------------

void StateDependencyTracker::buildTableByNameMap() {
    // Walk every P4Table and record controlPlaneName() → IR::P4Table*.
    struct Collector : Inspector {
        std::unordered_map<cstring, const IR::P4Table *> &out;
        explicit Collector(std::unordered_map<cstring, const IR::P4Table *> &o) : out(o) {}
        bool preorder(const IR::P4Table *tbl) override {
            out.emplace(tbl->controlPlaneName(), tbl);
            return true;
        }
    } col(tableByName_);
    programInfo.getP4Program().apply(col);
}

bool StateDependencyTracker::isTableVisited(
        cstring controlPlaneName, const P4::Coverage::CoverageSet &visited) const {
    auto it = tableByName_.find(controlPlaneName);
    if (it == tableByName_.end()) return false;
    // TableStepper::eval() calls markVisited(table) so a direct pointer lookup suffices.
    return visited.count(it->second) > 0;
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

// RAII helper — saves/restores SymbexOptions fields around a phase run.
// If sinkTableName is non-empty, temporarily adds it to skippedControlPlaneEntities
// so the table produces no synthesized entries (only its default action) during this phase.
// evalTableConstEntries() returns "always-miss" (true) when the table has no constant
// entries, so addDefaultAction takes the default unconditionally — no entry is emitted.
// extraSkippedTables lists additional tables to suppress entry generation for (e.g.
// size-1 tables whose single slot was already consumed by Phase 1).
struct ScopedSymbexOpts {
    bool savedOutputPacketOnly;
    // Forced true during SD execution so that IR::AssignmentStatement nodes
    // (e.g. inside RegisterAction::apply) are added to visitedNodes.
    bool savedCoverStatements;
    // Forced true so RegisterAction.execute() always creates a symbolic
    // TofinoRegisterValue test object and emits tofino_register_writeback for both
    // Phase 1 and Phase 2 (register reads in Phase 1 also update state).
    bool savedTamperingRegisterTracking;
    bool savedInitRegZeroValue;
    cstring sinkTableName_ = ""_cs;
    std::vector<cstring> extraSkippedTables_;
    ScopedSymbexOpts(bool setOutputPacketOnly, cstring sinkTableName = ""_cs,
                     std::vector<cstring> extraSkippedTables = {},
                     bool setRegTracking = true,
                     bool isPhase1 = false)
        : extraSkippedTables_(std::move(extraSkippedTables)) {
        auto &opts = SymbexOptions::get();
        savedOutputPacketOnly             = opts.outputPacketOnly;
        savedCoverStatements              = opts.coverageOptions.coverStatements;
        savedTamperingRegisterTracking    = opts.tamperingRegisterTracking;
        savedInitRegZeroValue        = opts.initRegZeroValue;
        opts.outputPacketOnly             = setOutputPacketOnly;
        opts.coverageOptions.coverStatements = true;
        opts.tamperingRegisterTracking    = setRegTracking;
        opts.initRegZeroValue        = isPhase1;
        sinkTableName_ = sinkTableName;
        if (!sinkTableName_.isNullOrEmpty()) {
            opts.skippedControlPlaneEntities.insert(sinkTableName_);
        }
        for (const auto &n : extraSkippedTables_) {
            opts.skippedControlPlaneEntities.insert(n);
        }
    }
    ~ScopedSymbexOpts() {
        auto &opts = SymbexOptions::get();
        opts.outputPacketOnly             = savedOutputPacketOnly;
        opts.coverageOptions.coverStatements     = savedCoverStatements;
        opts.tamperingRegisterTracking    = savedTamperingRegisterTracking;
        opts.initRegZeroValue        = savedInitRegZeroValue;
        if (!sinkTableName_.isNullOrEmpty()) {
            opts.skippedControlPlaneEntities.erase(sinkTableName_);
        }
        for (const auto &n : extraSkippedTables_) {
            opts.skippedControlPlaneEntities.erase(n);
        }
    }
};

void StateDependencyTracker::runTamperingScenario(const TamperingCallback &callBack,
                                                   const ExecutionState &initState) {
    // Build the controlPlanName → IR::P4Table* map once for this execution so that
    // allCovered can check whether a table's apply() was visited in visitedNodes.
    buildTableByNameMap();

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
        for (const auto *chain : chainList) {
            currentChain = chain;
            currentChainName = chainName;
            printInfo("============ Chain (%1%) id=%2% %3% [Tampering 3-phase] ============",
                      chainName, chain->id, chain->soName);

            // Per-chain cap on emitted sub-tests, reusing the existing --max-tests option.
            // Applied per chain (not globally) so every SOChain produces its own tests.
            // 0 means "unlimited": collect every valid Phase-1 × Phase-2 path.
            const size_t maxPerChain = static_cast<size_t>(SymbexOptions::get().maxTests);

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
            buildReachingSet();
            printInfo("[Tampering] Phase 1 (Read) — %1% required nodes", currentRequiredNodes.size());
            for (const auto *node : currentRequiredNodes)
                printInfo("  [%1%] %2% %3%", node->node_type_name(), node,
                          node->getSourceInfo().toPositionString());

            std::vector<const FinalState *> phase1States;
            {
                // TODO: Expected output packet can be unchanged or drop-to-fwd
                // isPhase1=true: zero-init registers so Z3 models hardware initial state (all-0).
                ScopedSymbexOpts guard(/*outputPacketOnly=*/true, ""_cs, {}, /*setRegTracking=*/true,
                                       /*isPhase1=*/true);
                auto &phase1Init = initState.clone();
                runPhase(phase1Init, phase1States, maxPerChain);
            }
            // Keep only Phase 1 states where the sink table HIT.
            // evalTableControlEntries sets tableHitVar to a concrete true/false in each
            // branch; addDefaultAction (miss path) sets it to false.  We evaluate the
            // value from the final model and discard miss states.
            if (!chain->sinkTableControlPlaneName.isNullOrEmpty()) {
                auto tblIt = tableByName_.find(chain->sinkTableControlPlaneName);
                if (tblIt != tableByName_.end()) {
                    const auto &hitVar = TableStepper::getTableHitVar(tblIt->second);
                    phase1States.erase(
                        std::remove_if(phase1States.begin(), phase1States.end(),
                            [&hitVar](const FinalState *fs) {
                                const auto *hitExpr =
                                    fs->getExecutionState()->get(hitVar);
                                if (hitExpr == nullptr) return true;
                                const auto *hitVal =
                                    fs->getFinalModel().evaluate(hitExpr, true);
                                const auto *hitBool = hitVal->to<IR::BoolLiteral>();
                                return hitBool == nullptr || !hitBool->value;
                            }),
                        phase1States.end());
                }
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
                if (SymbexOptions::get().distinctIOPorts) {
                    BUG_CHECK(cond.inputPort != cond.outputPort,
                              "Phase 1 identical input/output ports %1%", cond.inputPort);
                }
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
            buildReachingSet();
            printInfo("[Tampering] Phase 2 (Write) — %1% required nodes", currentRequiredNodes.size());
            for (const auto *node : currentRequiredNodes)
                printInfo("  [%1%] %2% %3%", node->node_type_name(), node,
                          node->getSourceInfo().toPositionString());

            // Store phase2States per phase1 condition bucket
            std::map<size_t, std::vector<const FinalState *>> phase2StateMap;
            size_t phase2StateNum = 0;

            for (size_t i = 0; i < phase1Conditions.size(); ++i) {
                const auto &cond1 = phase1Conditions[i];

                // Find a representative Phase 1 state for this condition bucket (used to
                // extract the evaluated TableConfig for size-1 tables below).
                const FinalState *repPhase1State = nullptr;
                for (size_t k = 0; k < phase1States.size(); ++k) {
                    if (phase1StateToCondition[k] == i) {
                        repPhase1State = phase1States[k];
                        break;
                    }
                }

                // Identify size-1 tables whose single slot was already consumed by Phase 1.
                // Phase 2 must not synthesize a new (different) entry for these tables; instead
                // Phase 1's pre-existing entry is injected into phase2Init so that Phase 2
                // evaluates it as HIT or MISS without generating a second control-plane entry.
                std::vector<cstring> size1Tables;
                for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
                    auto tblIt = tableByName_.find(tblName);
                    if (tblIt == tableByName_.end()) continue;
                    const auto *sizeConst = tblIt->second->getSizeProperty();
                    if (sizeConst != nullptr && sizeConst->asInt() == 1) {
                        size1Tables.push_back(tblName);
                        printInfo("[Tampering] Phase 2: size-1 table '%1%': "
                                  "reusing Phase 1 entry (no new entry generated)", tblName);
                    }
                }

                // Exclude the sink table from Phase 2's synthesized entries: the attacker's
                // write packet must not install a control-plane entry in the very table that
                // reads the tampered register value, as that entry would belong to Phase 1/3.
                // Also exclude size-1 tables whose slot is already occupied by Phase 1's entry.
                ScopedSymbexOpts guard(/*outputPacketOnly=*/false, chain->sinkTableControlPlaneName,
                                       size1Tables);
                auto &phase2Init = initState.clone();

                // Inject Phase 1's evaluated TableConfig for each size-1 table into Phase 2's
                // initial state so that evalTableConstEntries() can evaluate it as a
                // pre-existing entry (HIT/MISS) rather than creating a fresh symbolic entry.
                if (repPhase1State != nullptr) {
                    const auto &model1 = repPhase1State->getFinalModel();
                    const auto *es1 = repPhase1State->getExecutionState();
                    for (const auto &tblName : size1Tables) {
                        const auto *tblObj =
                            es1->getTestObject("tableconfigs"_cs, tblName, /*checked=*/false);
                        if (tblObj == nullptr) continue;
                        const auto *evalCfg =
                            tblObj->evaluate(model1, /*doComplete=*/true)->to<TableConfig>();
                        if (evalCfg != nullptr)
                            // Use a separate category so this entry is visible to
                            // evalTablePreExistingConfig but not emitted in Phase 2's test output
                            // (processPhase only reads "tableconfigs").
                            phase2Init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
                    }

                    // Carry Phase 1's register writes into Phase 2's initial state. Hardware
                    // runs Phase 1 → Phase 2 on the same device without clearing registers
                    // between phases, so Phase 2 reads whatever Phase 1 wrote. Without this,
                    // each runPhase zero-inits registers and Phase 2 accepts write paths gated
                    // on a prior register value Phase 1 actually determined (e.g.
                    // set_key_if_not_active's `prev != 0` gate depends on the access_bit that
                    // Phase 1's check_key set). evaluateForCarry() folds Phase 1's writes into
                    // the register's initialValue so initializeRegisterParameters resolves the
                    // Phase-2 read to the post-Phase-1 contents, matching hardware and pruning
                    // unreachable write paths.
                    for (const auto &[regName, regObj] :
                             es1->getTestObjectCategory("registervalues"_cs)) {
                        const auto *carried = regObj->evaluateForCarry(model1);
                        phase2Init.addTestObject("registervalues"_cs, regName, carried);
                    }
                }

                // Constrain Phase 2's input port to differ from Phase 1's input AND output
                phase2Init.pushPathConstraint(
                    new IR::Neq(inputPortSymExpr,
                                IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
                phase2Init.pushPathConstraint(
                    new IR::Neq(inputPortSymExpr,
                                IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
                // Constrain Phase 2's table match keys to differ from Phase 1's, so the two
                // phases produce compatible (non-conflicting) table entries that can coexist.
                // Skip size-1 tables: their entry is pre-injected and no new Neq is needed.
                const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
                for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
                    if (size1Set.count(tblName) > 0) continue;
                    for (const auto &[keyName, match] : keyMap) {
                        phase2Init.pushPathConstraint(
                            match->buildTableKeyNeqConstraint(tblName, keyName));
                    }
                }
                runPhase(phase2Init, phase2StateMap[i], maxPerChain);
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
            // Round-robin emission across Phase-1 states so the per-chain cap
            // (maxPerChain) never starves a later Phase-1 read-state: every Phase-1
            // state contributes at least one sub-test before any state gets a second.
            // The discriminating write path (the one that actually drives the register
            // write) may only be reachable under a specific Phase-1 read-state, so
            // draining the whole budget on the first state's Phase-2 paths could
            // otherwise skip it entirely.
            size_t subTestId = 0;
            std::vector<size_t> cursor(phase1States.size(), 0);
            bool chainCapHit = false;
            while (!chainCapHit) {
                bool emittedThisRound = false;
                for (size_t i = 0; i < phase1States.size() && !chainCapHit; ++i) {
                    auto &fs2List = phase2StateMap[phase1StateToCondition[i]];
                    if (cursor[i] >= fs2List.size()) continue;  // Phase-1 state exhausted
                    const auto *fs1 = phase1States[i];
                    const auto &cond1 = phase1Conditions[phase1StateToCondition[i]];
                    const auto *fs2 = fs2List[cursor[i]++];
                    emittedThisRound = true;
                    // Derive attacker-chosen register values from Phase 2.
                    // withAttackerValues() is called on the *unevaluated* register object so
                    // that symbolic write expressions are still available to build constraints.
                    // It returns (a) the register seeded with concrete attacker-chosen values
                    // and (b) model overrides applied to processPhase(phase2) so the emitted
                    // Phase 2 input packet shows the attacker-chosen value.
                    std::map<cstring, const TestObject *> attackerRegValues;
                    std::map<cstring, cstring> attackerRegSinkTables;
                    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>>
                        phase2ModelOverrides;
                    // Collect the Phase-1 key value for only the specific sink-table key
                    // that the register value flows into (sinkKeyName at sinkTableControlPlaneName).
                    // The attacker-chosen register value must MISS exactly at that key in Phase 3.
                    std::vector<big_int> forbiddenValues;
                    const cstring sinkTableJoined = chain->sinkTableControlPlaneName;
                    if (!sinkTableJoined.isNullOrEmpty() && !chain->sinkKeyName.isNullOrEmpty()) {
                        auto tblIt = cond1.tableKeyMap.find(sinkTableJoined);
                        if (tblIt != cond1.tableKeyMap.end()) {
                            auto keyIt = tblIt->second.find(chain->sinkKeyName);
                            if (keyIt != tblIt->second.end()) {
                                const auto *reprVal = keyIt->second->getRepresentativeValue();
                                if (reprVal != nullptr)
                                    forbiddenValues.push_back(reprVal->value);
                            }
                        }
                    }
                    for (const auto &[regName, regObj] :
                             fs2->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
                        if (regName != chain->soName) continue;
                        auto [attackerValue, overrides] =
                            regObj->withAttackerValues(fs2->getFinalModel(),
                                                       SymbexOptions::get().stateTamperValue,
                                                       forbiddenValues);
                        attackerRegValues[regName] = attackerValue;
                        if (!sinkTableJoined.isNullOrEmpty()) {
                            attackerRegSinkTables[regName] = sinkTableJoined;
                        }
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

                    // Build selective NEQ constraints for Phase 1's re-solve in processPhase.
                    // These prevent Z3 from assigning Phase 1's packet fields / control-plane
                    // keys to the same concrete values Phase 2 already used, which would
                    // create conflicting table entries or cause Phase 1 to unexpectedly hit
                    // a Phase 2-installed entry at runtime.
                    std::vector<const IR::Expression *> p1ExtraConstraints;
                    {
                        auto cond2 = buildPhaseCondition(*fs2, programInfo);
                        // Re-derive the set of size-1 tables for this condition bucket
                        // so we can skip them (their entry is shared via preexisting_tableconfigs).
                        std::set<cstring> size1Set;
                        for (const auto &[tblName, _km] : cond1.tableKeyMap) {
                            auto tblIt = tableByName_.find(tblName);
                            if (tblIt == tableByName_.end()) continue;
                            const auto *sizeConst = tblIt->second->getSizeProperty();
                            if (sizeConst != nullptr && sizeConst->asInt() == 1)
                                size1Set.insert(tblName);
                        }
                        for (const auto &[tblName, keyMap2] : cond2.tableKeyMap) {
                            // Skip tables already handled by the pre-existing-configs mechanism
                            // or tables Phase 2 was explicitly prohibited from entering.
                            if (size1Set.count(tblName) > 0 ||
                                    tblName == chain->sinkTableControlPlaneName)
                                continue;
                            for (const auto &[keyName, match2] : keyMap2) {
                                if (cond1.tableKeyMap.count(tblName) > 0) {
                                    // Phase 1 HIT this table: prevent the re-solve from picking
                                    // the same control-plane key as Phase 2 (conflicting entries).
                                    p1ExtraConstraints.push_back(
                                        match2->buildTableKeyNeqConstraint(tblName, keyName));
                                } else {
                                    // Phase 1 MISSED this table but Phase 2 hit it.
                                    // Prevent Phase 1's packet from matching Phase 2's entry.
                                    auto tblIt = tableByName_.find(tblName);
                                    if (tblIt == tableByName_.end()) continue;
                                    const auto *tblIR = tblIt->second;
                                    if (tblIR->getKey() == nullptr) continue;
                                    for (const auto *keyElem : tblIR->getKey()->keyElements) {
                                        const auto *nameAnnot = keyElem->getAnnotation(
                                            IR::Annotation::nameAnnotation);
                                        if (nameAnnot == nullptr ||
                                                nameAnnot->getName() != keyName)
                                            continue;
                                        // Resolve the packet-field expression through Phase 1's
                                        // execution state to get the symbolic variable.
                                        const auto stateVar = ToolsVariables::convertReference(
                                            keyElem->expression);
                                        if (!fs1->getExecutionState()->exists(stateVar))
                                            continue;
                                        const auto *pktField =
                                            fs1->getExecutionState()->get(stateVar);
                                        p1ExtraConstraints.push_back(
                                            match2->buildPacketFieldNeqConstraint(pktField));
                                    }
                                }
                            }
                        }
                        // Size-1 tables: pin Phase-1's emitted control-plane key to the SAME
                        // value the Phase-2 preexisting fork constrained the packet against
                        // (cond1's value V). The single slot persists across phases, so the
                        // installed entry key (== Phase-1 packet key on HIT) and the Phase-2
                        // packet key must agree on V; otherwise Phase-1's free re-solve picks a
                        // different key (e.g. 0) that collides with Phase-2's packet key, making
                        // the slot HIT in Phase 2 on hardware (the switchv2p match_gw/to_gw bug).
                        for (const auto &tblName : size1Set) {
                            auto cIt = cond1.tableKeyMap.find(tblName);
                            if (cIt == cond1.tableKeyMap.end()) continue;
                            for (const auto &[keyName, match1] : cIt->second) {
                                // Pin the emitted control-plane key to Phase 1's concrete match
                                // for every match kind (exact/ternary/lpm/range/optional); the
                                // per-type override also pins mask/prefix/high so the single slot
                                // is fully determined.
                                p1ExtraConstraints.push_back(
                                    match1->buildTableKeyEqConstraint(tblName, keyName));
                            }
                        }
                    }

                    // Phase 3 is purely dynamic: the test script replays Phase 1's packet after
                    // Phase 2 writes the attacker-chosen value to the register.
                    TamperingFinalState ts{*fs1, *fs2, false,
                                           cond1.inputPort, cond1.outputPort,
                                           ip2, op2,
                                           attackerRegValues, phase2ModelOverrides,
                                           attackerRegSinkTables,
                                           p1ExtraConstraints};
                    ts.chainId = chain->id;
                    ts.subTestId = ++subTestId;
                    callBack(ts);
                    if (maxPerChain != 0 && subTestId >= maxPerChain) chainCapHit = true;
                }
                // A full sweep that emitted nothing means every Phase-1 state's Phase-2
                // paths are exhausted: this chain is done.
                if (!emittedThisRound) break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Single-phase DFS helper
// ---------------------------------------------------------------------------

void StateDependencyTracker::runPhase(ExecutionState &phaseInit,
                                       std::vector<const FinalState *> &out,
                                       size_t maxStates) {
    unexploredBranches.clear();
    // phaseInit is a caller-owned clone. The caller is responsible for pushing any
    // Z3 path constraints (e.g., port equality/exclusion from Phase 1's symbolic
    // variable) before calling this function.
    runImpl([&out, maxStates, this](const FinalState &fs) -> bool {
        const auto *es = fs.getExecutionState();
        const auto &opts = SymbexOptions::get();

        if (opts.outputPacketOnly &&
            (es->getPacketBufferSize() <= 0 || es->getProperty<bool>("drop"_cs))) {
            return false;
        }

        // distinctIOPorts: always a terminal-state check because the output port is
        // assigned during execution (not a free initial symbolic variable).
        // Skip when either port is tainted — a tainted port means the path does not
        // constrain the port value, so we cannot evaluate distinctness.
        if (opts.distinctIOPorts) {
            const auto *ipExpr = es->get(programInfo.getTargetInputPortVar());
            const auto *opExpr = es->get(programInfo.getTargetOutputPortVar());
            if (!Taint::hasTaint(ipExpr) && !Taint::hasTaint(opExpr)) {
                const auto &model = fs.getFinalModel();
                auto inputPort = IR::getIntFromLiteral(model.evaluate(ipExpr, true));
                auto outputPort = IR::getIntFromLiteral(model.evaluate(opExpr, true));
                if (inputPort == outputPort) {
                    printInfo("[SDTrack DEBUG] Phase1 state rejected by distinctIOPorts: "
                              "in=%1% == out=%2%", inputPort, outputPort);
                    return false;
                }
                printInfo("[SDTrack DEBUG] Phase1 state accepted: in=%1% out=%2%",
                          inputPort, outputPort);
            }
        }

        out.push_back(new FinalState(fs));
        // Returning true tells runImpl to stop (terminate the DFS). When maxStates is 0
        // we never stop early, so the guided DFS keeps backtracking and collects every
        // valid terminal path; otherwise we stop once maxStates paths are collected.
        return maxStates != 0 && out.size() >= maxStates;
    }, phaseInit);
}

// ---------------------------------------------------------------------------
// Guided DFS
// ---------------------------------------------------------------------------

void StateDependencyTracker::buildReachingSet() {
    reachingSet_.clear();
    reachingSetValid_ = false;
    // getCallGraph() BUGs if no DCG was built; --state-dep enables the DCG. Degrade gracefully
    // (legacy steering, no pruning) when it is absent.
    if (!SymbexOptions::get().dcg) return;

    const auto &dcg = programInfo.getCallGraph();
    const auto &dcgNodes = dcg.getNodes();
    const auto &inEdges = dcg.getInEdges();

    std::vector<const IR::Node *> work;
    auto seed = [&](const IR::Node *n) {
        if (n == nullptr || dcgNodes.find(n) == dcgNodes.end()) return;
        if (reachingSet_.insert(n).second) work.push_back(n);
    };

    // Seed from the required nodes that are control vertices in the DCG. RegisterAction-internal
    // nodes (the apply Function / its body statements) are not DCG vertices and need no seed: they
    // are reached transitively once the `.execute()` assignment (which IS a DCG vertex) is reached.
    // IR::Key required nodes are not DCG vertices either; seed from the owning sink table instead.
    for (const auto *req : currentRequiredNodes) {
        if (req == nullptr) continue;
        if (req->is<IR::Key>()) {
            if (currentChain != nullptr) {
                auto it = tableByName_.find(currentChain->sinkTableControlPlaneName);
                if (it != tableByName_.end()) seed(it->second);
            }
            continue;
        }
        seed(req);
    }
    if (reachingSet_.empty()) return;  // no usable seed → keep legacy behaviour

    // Backward BFS over predecessor edges: closure of all nodes that can reach a seed.
    while (!work.empty()) {
        const auto *n = work.back();
        work.pop_back();
        auto it = inEdges.find(n);
        if (it == inEdges.end() || it->second == nullptr) continue;
        for (const auto *pred : *it->second)
            if (reachingSet_.insert(pred).second) work.push_back(pred);
    }
    reachingSetValid_ = true;
}

std::optional<ExecutionStateReference> StateDependencyTracker::pickSuccessor(
    StepResult successors) {
    if (successors->empty()) return std::nullopt;
    if (successors->size() == 1) return successors->at(0).nextState;

    // The IR node a branch is about to execute (nullptr if its next command is not an IR node).
    auto branchNextNode = [](const auto &b) -> const IR::Node * {
        auto cmdOpt = b.nextState.get().getNextCmd();
        if (!cmdOpt.has_value()) return nullptr;
        if (const auto *np = std::get_if<const IR::Node *>(&*cmdOpt)) return *np;
        return nullptr;
    };
    // A branch already covers a required node via local lookahead or its visited set.
    auto hitsRequired = [this](const auto &b) {
        for (const auto *node : b.potentialNodes)
            if (currentRequiredNodes.count(node) != 0U) return true;
        for (const auto *node : b.nextState.get().getVisited())
            if (currentRequiredNodes.count(node) != 0U) return true;
        return false;
    };
    // Conservative viability: a branch is viable unless the reachability oracle proves its next
    // (tracked) control node cannot reach any required node. Untracked/unknown next nodes are kept.
    auto isViable = [&](const auto &b) {
        if (!reachingSetValid_) return true;             // no oracle → never prune (legacy)
        if (hitsRequired(b)) return true;                // already covers a required node
        const auto *next = branchNextNode(b);
        if (next == nullptr) return true;                // non-IR next command → can't reason
        if (reachingSet_.count(next) != 0U) return true;  // transitively reaches the target
        const auto &dcgNodes = programInfo.getCallGraph().getNodes();
        if (dcgNodes.find(next) == dcgNodes.end()) return true;  // untracked node → keep
        return false;                                    // tracked + unreachable → prune
    };

    // Prefer a branch that already hits a required node (closest); otherwise the first viable
    // branch (transitive steering toward the target chain).
    std::optional<size_t> chosenIdx;
    for (size_t i = 0; i < successors->size(); ++i) {
        if (hitsRequired(successors->at(i))) {
            chosenIdx = i;
            break;
        }
        if (!chosenIdx.has_value() && isViable(successors->at(i))) chosenIdx = i;
    }

    if (chosenIdx.has_value()) {
        auto chosen = successors->at(*chosenIdx);
        successors->erase(successors->begin() + static_cast<ptrdiff_t>(*chosenIdx));
        // Prune non-viable siblings: only branches that can still reach the target are kept on
        // the backtrack stack, so the DFS no longer wanders subtrees that provably cannot.
        for (auto &b : *successors)
            if (isViable(b)) unexploredBranches.push_back(b);
        return chosen.nextState;
    }

    // No viable branch (e.g. genuine dead end, or oracle unavailable): legacy random fallback.
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
                                [&visited, this](const IR::Node *n) {
                                    // IR::Key nodes are never passed to markVisited; instead
                                    // check whether the table that owns this key had its
                                    // apply() MethodCallStatement visited.
                                    if (n->is<IR::Key>()) {
                                        return isTableVisited(
                                            currentChain->sinkTableControlPlaneName, visited);
                                    }
                                    return visited.count(n) > 0;
                                });
                if (allCovered) {
                    printInfo("[SDTrack] Test found: chain=%1% id=%2% SO=%3% (%4% nodes)",
                                currentChainName, currentChain->id, currentChain->soName,
                                currentRequiredNodes.size());
                    for (const auto *node : currentRequiredNodes) {
                        printInfo("  OK  [%1%] %2% %3%",
                                    node->node_type_name(), node,
                                    node->getSourceInfo().toPositionString());
                    }
                    bool terminate = handleTerminalState(callBack, executionState);
                    if (terminate) return;
                } else {
                    // DEBUG: show which required nodes were not visited so the caller can
                    // distinguish a node-identity mismatch from a DFS coverage failure.
                    printInfo("[SDTrack DEBUG] Terminal state reached but allCovered=false "
                              "(%1% required nodes):", currentRequiredNodes.size());
                    for (const auto *n : currentRequiredNodes) {
                        bool hit = n->is<IR::Key>()
                            ? isTableVisited(currentChain->sinkTableControlPlaneName, visited)
                            : visited.count(n) > 0;
                        printInfo("  %1% [%2%] %3% %4%", (hit ? "OK  " : "MISS"),
                                  n->node_type_name(), n,
                                  n->getSourceInfo().toPositionString());
                    }
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
        } catch (Util::CompilerBug &e) {
            // Collecting *every* valid path (runPhase with maxStates>1) makes the guided
            // DFS explore far more of the state space than the previous first-match-wins
            // behaviour did. Some of those paths hit modeling gaps in the shared symbex
            // engine (e.g. a tainted Mux of unknown type reaching ExecutionState::set()),
            // which are raised as BUG_CHECK/CompilerBug. A single unmodelable path must not
            // abort the whole multi-path search: log it and backtrack like an unimplemented
            // feature, so the remaining (modelable) paths — including the discriminating
            // write path — are still collected and emitted. --strict re-throws for debugging.
            if (SymbexOptions::get().strict) throw;
            warning("[SDTrack] Skipping path that triggered an internal error during guided "
                    "DFS. Message: %1%\n", e.what());
        }

        // Backtrack (LIFO).
        if (unexploredBranches.empty()) return;
        Util::ScopedTimer chooseBranchTimer("branch_selection");
        executionState = unexploredBranches.back().nextState;
        unexploredBranches.pop_back();
    }
}

}  // namespace P4::P4Tools::Symbex
