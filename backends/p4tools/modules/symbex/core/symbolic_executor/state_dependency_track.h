#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_

#include <functional>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

#include "ir/ir.h"
#include "lib/cstring.h"
#include "midend/coverage.h"

#include "backends/state_dependency/analysis.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/lib/final_state.h"
#include "backends/p4tools/modules/symbex/lib/test_object.h"

namespace P4::P4Tools::Symbex {

enum class StateDependencyPolicy {
    Tampering,
    AlteringPath,
};

/// Tracks which phase of the three-packet tampering scenario is being executed.
enum class TamperingPhase {
    Phase1_Read,   ///< First packet: read original register value.
    Phase2_Write,  ///< Second packet: write malicious value to register.
    Phase3_Read,   ///< Third packet: read changed value (register pre-set from Phase 2).
};

/// Bundles the two symbex FinalStates that form one tampering test case:
///   phase1 — packet that reads the original register value
///   phase2 — packet that writes the tampered value
///   (Phase 3 is purely dynamic: the test script replays Phase 1's packet after Phase 2.)
///
/// readPathHasExit is true when the SOChain's read vertices include __EXIT__ leaves,
/// meaning the read phases must produce an output packet (packet is not dropped).
///
/// The phaseN{Input,Output}Port fields are concrete port values extracted from each
/// phase's original final model in runTamperingScenario. They are passed to
/// processPhase() as overrides because computeConcolicState() re-solves and may
/// assign different (but equally valid) concrete values for the port variables.
struct TamperingFinalState {
    const FinalState &phase1;
    const FinalState &phase2;
    bool readPathHasExit = false;
    int phase1InputPort = -1;
    int phase1OutputPort = -1;
    int phase2InputPort = -1;
    int phase2OutputPort = -1;
    /// Attacker-chosen register values (random by default) generated from Phase 2.
    /// Maps register control-plane name → evaluated TestObject with concrete values.
    /// Populated by runTamperingScenario(); emitted as affected_register fields in test output.
    std::map<cstring, const TestObject *> attackerRegisterValues;
    /// Direct model overrides for Phase 2: each (SymbolicVariable, Constant) pair is applied
    /// via Model::set() after computeConcolicState() so that the emitted Phase 2 input packet
    /// shows the attacker-chosen value in the field that is written to the register.
    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>> phase2ModelOverrides;
    /// Per-register sink-table information for Key-sink chains. Maps register
    /// control-plane name → comma-separated list of Phase-1 sink-table control-plane
    /// names whose installed keys must MISS when Phase 3 replays Phase 1's input.
    /// Empty (or register not in map) for non-Key-sink chains. Emitted as
    /// `sink_table` + `hit_phase=1` + `miss_phase=3` metadata on affected_register.
    std::map<cstring, cstring> attackerRegisterSinkTables;
    /// Extra path constraints for processPhase(phase1)'s computeConcolicState.
    /// NEQ constraints derived from Phase 2's concrete table keys to prevent
    /// Z3 from re-assigning Phase 1's packet fields to Phase 2's key values.
    std::vector<const IR::Expression *> phase1ExtraConstraints;
    /// SOChain id this test case belongs to. Used to name the emitted file
    /// basePath_<chainId>_<subTestId>. Assigned in runTamperingScenario.
    size_t chainId = 0;
    /// Per-chain sub-test index (1-based), distinguishing the multiple valid
    /// Phase-1 × Phase-2 paths of a single SOChain. Assigned in runTamperingScenario.
    size_t subTestId = 0;
};

/// Callback type for the three-phase tampering scenario.
using TamperingCallback = std::function<bool(const TamperingFinalState &)>;

/// Symbolic executor that steers path selection toward SOChains from state-dependency
/// analysis. For each SOChain the executor runs an independent DFS from the initial
/// program state, preferring branches that cover the chain's required IR nodes. A test
/// vector is emitted only when a terminal path covers ALL required nodes.
///
/// Policy variants map to different chain sources and node sets:
///   - Tampering    : dataWriteKeyChains
///                    Three-phase execution per chain:
///                      Phase 1 → readNodes  (read original value)
///                      Phase 2 → writeNodes (write tampered value)
///                      Phase 3 → readNodes  (read tampered value; register pre-set from Phase 2)
///   - AlteringPath : dataWriteCondChains → writeNodes + readNodes
class StateDependencyTracker : public SymbolicExecutor {
 public:
    StateDependencyTracker(AbstractSolver &solver, const ProgramInfo &programInfo,
                           const P4StateDependency::StateDependencyResult &sdResult,
                           StateDependencyPolicy policy);

    /// Guided DFS targeting currentRequiredNodes for a single chain.
    void runImpl(const Callback &callBack, ExecutionStateReference executionState) override;

    /// Three-phase entry point for the Tampering policy.
    /// Emits one TamperingFinalState per found chain triple.
    void runTampering(const TamperingCallback &callBack);

    /// Returns the active policy (used by callers to choose the right entry point).
    [[nodiscard]] StateDependencyPolicy getPolicy() const { return policy; }

 private:
    const P4StateDependency::StateDependencyResult &sdResult;
    const StateDependencyPolicy policy;

    /// Which phase is currently being executed (Tampering policy only).
    TamperingPhase currentPhase = TamperingPhase::Phase1_Read;

    /// IR nodes that the current chain requires the path to cover.
    P4::Coverage::CoverageSet currentRequiredNodes;

    /// Directed-search reachability oracle: every IR node from which some node in
    /// currentRequiredNodes is control-reachable (backward closure over the program DCG).
    /// A branch whose next IR node is absent from this set provably cannot reach the target
    /// chain and is pruned. Recomputed per phase by buildReachingSet().
    std::unordered_set<const IR::Node *> reachingSet_;

    /// True once reachingSet_ is a usable oracle for the current phase. When false (no DCG was
    /// built, or a required-node seed could not be resolved to a DCG vertex), pickSuccessor
    /// falls back to the legacy potentialNodes/visited steering and prunes nothing.
    bool reachingSetValid_ = false;

    /// Builds reachingSet_ from currentRequiredNodes via a single backward BFS over the program
    /// DCG's predecessor edges. No-op (leaves reachingSetValid_ = false) when no DCG is available.
    void buildReachingSet();

    /// The SOChain being explored (set in run(), read in runImpl()).
    const P4StateDependency::DependencyGraphs::SOChain *currentChain = nullptr;

    /// The current chain's sink table (the table whose key the SO value feeds), or nullptr for
    /// chains without a Key sink. Set before Phase 1; used by pickSuccessor to prefer the sink
    /// table's HIT branch so the table lookup actually matches in Phase 1.
    const IR::P4Table *currentSinkTable_ = nullptr;

    /// Name of the current chain category (e.g. "Write Condition"), set in run().
    cstring currentChainName;

    /// DFS backtrack stack for the current chain exploration.
    std::vector<Branch> unexploredBranches;

    /// Returns a flat list of SOChain pointers selected by the policy, grouped by chain category.
    std::map<cstring, std::vector<const P4StateDependency::DependencyGraphs::SOChain *>>
    collectChains() const;

    /// Builds the CoverageSet of required IR nodes for one SOChain per the policy.
    /// For Tampering, the set depends on currentPhase.
    P4::Coverage::CoverageSet buildRequiredNodes(
        const P4StateDependency::DependencyGraphs::SOChain &chain) const;

    /// Runs a single-phase DFS from phaseInit (a caller-owned clone with any required path
    /// constraints already pushed), saving each accepted terminal FinalState into out.
    /// @param maxStates caps the number of collected states; 0 means collect every valid
    /// path (the DFS keeps backtracking until the search space is exhausted).
    void runPhase(ExecutionState &phaseInit, std::vector<const FinalState *> &out,
                  size_t maxStates);

    /// Orchestrates Phase 1 → Phase 2 → Phase 3 for every SOChain and fires callBack.
    void runTamperingScenario(const TamperingCallback &callBack, const ExecutionState &initState);

    /// Returns true when any read-side vertex in the chain maps to an EXIT/ENTRY ESG node
    /// (i.e. a dep-graph vertex whose corresponding ESG node has a null IR::Node*).
    /// This indicates the read path must reach program exit (packet is not dropped).
    static bool chainHasExitLeaf(const P4StateDependency::DependencyGraphs::SOChain &chain);

    /// Picks the next execution state from candidate successors, preferring branches
    /// whose potentialNodes or already-visited nodes intersect currentRequiredNodes.
    /// Falls back to random selection when no matching branch is found.
    std::optional<ExecutionStateReference> pickSuccessor(StepResult successors);

    /// Maps each table's control-plane name to its IR::P4Table*.
    /// Built once per execution; used by allCovered to resolve IR::Key required nodes
    /// (which are never markVisited'd) to their owning table (which is markVisited'd).
    std::unordered_map<cstring, const IR::P4Table *> tableByName_;

    /// Builds tableByName_ by traversing the P4 program once.
    void buildTableByNameMap();

    /// Returns true if the table with the given control-plane name was visited.
    bool isTableVisited(cstring controlPlaneName,
                        const P4::Coverage::CoverageSet &visited) const;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_ */
