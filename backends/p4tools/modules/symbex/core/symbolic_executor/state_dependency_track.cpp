#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "ir/ir.h"
#include "ir/solver.h"
#include "lib/error.h"
#include "lib/timer.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

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
// Non-tampering single-chain DFS (original behaviour)
// ---------------------------------------------------------------------------

void StateDependencyTracker::run(const Callback &callBack) {
    auto chains = collectChains();
    if (chains.empty()) {
        warning("State-dependency analysis produced no chains for the selected policy.");
        return;
    }

    auto &initState = ExecutionState::create(&programInfo.getP4Program());

    if (policy == StateDependencyPolicy::Tampering) {
        // Delegate to the three-phase scenario; bridge each triple to the legacy
        // single-FinalState callback by emitting Phase 3 as the representative result.
        runTamperingScenario([&callBack](const TamperingFinalState &ts) -> bool {
            return callBack(ts.phase3);
        }, initState);
        return;
    }

    for (const auto &[chainName, chainList] : chains) {
        for (const auto *chain : chainList) {
            currentChain = chain;
            currentChainName = chainName;
            currentRequiredNodes = buildRequiredNodes(*chain);
            if (currentRequiredNodes.empty()) continue;

            unexploredBranches.clear();
            printInfo("============ Chain (%1%) id=%2% SO=%3% ============",
                chainName, chain->id, chain->soName);
            runImpl(callBack, initState.clone());
        }
    }
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

void StateDependencyTracker::runTamperingScenario(const TamperingCallback &callBack,
                                                   const ExecutionState &initState) {
    auto chains = collectChains();

    for (const auto &[chainName, chainList] : chains) {
        for (const auto *chain : chainList) {
            currentChain = chain;
            currentChainName = chainName;

            printInfo(
                "============ Chain (%1%) id=%2% %3% [Tampering 3-phase] ============",
                chainName, chain->id, chain->soName);

            // ---- Phase 1: read original register value ----
            phase1States.clear();
            currentPhase = TamperingPhase::Phase1_Read;
            currentRequiredNodes = buildRequiredNodes(*chain);
            if (currentRequiredNodes.empty()) {
                warning("[Tampering] Chain id=%1% has no readNodes; skipping.", chain->id);
                continue;
            } else {
                for (const auto *node : currentRequiredNodes) {
                    printInfo("  [%1%] %2% %3%",
                                node->node_type_name(), node,
                                node->getSourceInfo().toPositionString());
                }
            }
            printInfo("[Tampering] Phase 1 (Read) — %1% required nodes", currentRequiredNodes.size());
            {
                // When the chain's read path includes EXIT leaves, only accept terminal
                // states that produce an output packet — same semantics as --output-packet-only.
                auto &opts = SymbexOptions::get();
                bool savedOutputPacketOnly = opts.outputPacketOnly;
                if (chainHasExitLeaf(*chain)) opts.outputPacketOnly = true;
                runPhase(initState, phase1States);
                opts.outputPacketOnly = savedOutputPacketOnly;
            }
            if (phase1States.empty()) {
                warning("[Tampering] Phase 1 found no terminal state for chain id=%1%.", chain->id);
                continue;
            }

            // ---- Phase 2: write tampered value ----
            phase2States.clear();
            currentPhase = TamperingPhase::Phase2_Write;
            currentRequiredNodes = buildRequiredNodes(*chain);
            if (currentRequiredNodes.empty()) {
                warning("[Tampering] Chain id=%1% has no writeNodes; skipping.", chain->id);
                continue;
            }
            printInfo("[Tampering] Phase 2 (Write) — %1% required nodes", currentRequiredNodes.size());
            runPhase(initState, phase2States);
            if (phase2States.empty()) {
                warning("[Tampering] Phase 2 found no terminal state for chain id=%1%.", chain->id);
                continue;
            }

            // ---- Phase 3: read tampered value ----
            // Build a fresh initial state with the register pre-set to Phase 2's written value.
            // This reuses the same execution path as Phase 1 but starts with the tampered
            // register, so the output packet reflects the changed value.
            for (const auto *fs2 : phase2States) {
                auto &phase3Init = initState.clone();

                // Copy Phase 2's evaluated (concrete) register test objects into the Phase 3
                // initial state so the symbex engine sees the tampered value from the start.
                const auto &p2Registers =
                    fs2->getExecutionState()->getTestObjectCategory("register_values"_cs);
                for (const auto &[regName, regObj] : p2Registers) {
                    const auto *evaluated = regObj->evaluate(fs2->getFinalModel(), /*doComplete=*/true);
                    phase3Init.addTestObject("register_values"_cs, regName, evaluated);
                }

                std::vector<const FinalState *> phase3States;
                currentPhase = TamperingPhase::Phase3_Read;
                currentRequiredNodes = buildRequiredNodes(*chain);  // = readNodes
                printInfo("[Tampering] Phase 3 (ReadTampered) — %1% required nodes",
                          currentRequiredNodes.size());
                {
                    auto &opts = SymbexOptions::get();
                    bool savedOutputPacketOnly = opts.outputPacketOnly;
                    if (chainHasExitLeaf(*chain)) opts.outputPacketOnly = true;
                    runPhase(phase3Init, phase3States);
                    opts.outputPacketOnly = savedOutputPacketOnly;
                }

                if (phase3States.empty()) {
                    warning("[Tampering] Phase 3 found no terminal state for chain id=%1%.", chain->id);
                    continue;
                }

                // Emit one triple per (phase1, phase2, phase3) combination.
                bool hasExit = chainHasExitLeaf(*chain);
                for (const auto *fs1 : phase1States) {
                    for (const auto *fs3 : phase3States) {
                        TamperingFinalState ts{*fs1, *fs2, *fs3, hasExit};
                        if (callBack(ts)) return;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Single-phase DFS helper
// ---------------------------------------------------------------------------

void StateDependencyTracker::runPhase(const ExecutionState &initState,
                                       std::vector<const FinalState *> &out) {
    unexploredBranches.clear();
    // Use an inner callback that saves terminal states instead of forwarding them out.
    // When outputPacketOnly is set (possibly temporarily by runTamperingScenario for read phases),
    // reject dropped-packet terminal states and continue exploring — same semantics as run().
    runImpl([&out](const FinalState &fs) -> bool {
        const auto *es = fs.getExecutionState();
        if (SymbexOptions::get().outputPacketOnly &&
            (es->getPacketBufferSize() <= 0 || es->getProperty<bool>("drop"_cs))) {
            return false;  // reject; keep backtracking
        }
        out.push_back(new FinalState(fs));  // copy-construct to heap for cross-phase lifetime
        return out.size() >= 1;             // stop after the first accepted result
    }, initState.clone());
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
