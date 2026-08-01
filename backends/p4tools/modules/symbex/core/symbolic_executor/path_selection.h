#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_PATH_SELECTION_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_PATH_SELECTION_H_

#include <set>

namespace P4::P4Tools::Symbex {

enum class PathSelectionPolicy {
    DepthFirst,
    RandomBacktrack,
    GreedyStmtCoverage,
    StateDependencyTampering,
    StateDependencyTamperingCond,
};

/// Which tampering search phases use one global (shared) DFS steered at the union of all chains'
/// target nodes, with per-chain assignment deferred until terminals are bucketed. Cumulative: PHASE2
/// is only meaningful on top of PHASE1, so the enum makes "Phase 2 only" unexpressible.
enum class SharedTraversalMode {
    /// Per-chain Phase-1 traversal (the pre-sharing behaviour). Measurement baseline only — it
    /// re-traverses the program once per chain.
    None,
    /// One shared Phase-1 read-path traversal; Phase 2 runs the per-chain constrained loop.
    Phase1,
    /// Shared Phase-1, plus a global Phase-2 write-path pass that prunes chains whose write nodes
    /// are unreachable before the expensive constrained loop.
    Phase1Phase2,
};

inline bool requiresLookahead(PathSelectionPolicy &pathSelectionPolicy) {
    static const std::set LOOKAHEAD_STRATEGYIES = {
        PathSelectionPolicy::GreedyStmtCoverage,
        PathSelectionPolicy::StateDependencyTampering,
        PathSelectionPolicy::StateDependencyTamperingCond,
    };
    return LOOKAHEAD_STRATEGYIES.find(pathSelectionPolicy) != LOOKAHEAD_STRATEGYIES.end();
}

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_PATH_SELECTION_H_ */
