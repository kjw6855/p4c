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

    // ---- Tamper direction (sink-flip observable) ---------------------------------------------
    /// Tamper direction. false = HIT→MISS (Phase-1 sink HIT, Phase-3 MISS); true = MISS→HIT
    /// (Phase-1 sink MISS, Phase-3 HIT). Selects how hit_phase/miss_phase are emitted. Phase 3 is a
    /// dynamic deviation check for both directions — the end-to-end validator compares the Phase-3
    /// output to the Phase-1 reference, so p4symbex emits no predicted Phase-3 disposition.
    bool missToHit = false;
    /// Human-readable case label, e.g. "MISS_TO_HIT/FWD_TO_DROP" (set for MISS→HIT, from the
    /// flip-confirmation run). Emitted as informational metadata; empty for HIT→MISS.
    cstring caseLabel = ""_cs;

    // ---- Multicast forward (over-approximated as a single representative port) ----------------
    /// True if the forwarding phase reached a non-zero multicast group (mcast_grp), i.e. the
    /// modeled forward came from multicast rather than a unicast egress port. When set, the
    /// emitted test carries a multicast_group block so the p4csd validator installs the group
    /// (mgid → representative port) before replay and removes it after.
    bool usesMulticast = false;
    /// The concrete multicast group id the packet carries on the forwarding path; valid only
    /// when usesMulticast is true.
    int multicastGroupId = -1;
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

    /// True while runMissToHitChain collects Phase-1 sink-MISS baselines. Disables pickSuccessor's
    /// sink-HIT steering (which would otherwise pull Phase 1 onto HIT branches we discard).
    bool seekMiss_ = false;

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

    /// Orchestrates the per-chain three-phase scenario for every SOChain (both tamper directions)
    /// and fires callBack.
    void runTamperingScenario(const TamperingCallback &callBack, const ExecutionState &initState);

    /// Runs the three-phase scenario for one SOChain in one tamper direction, sharing the Phase-1
    /// collection and Phase-2 write between directions. @p missToHit selects the direction:
    ///   false (HIT→MISS): keep Phase-1 sink-HIT terminals; Phase 3 is a dynamic deviation check
    ///                     (emit hit_phase=1 / miss_phase=3).
    ///   true  (MISS→HIT): keep reached-sink-MISS terminals; symbolically replay Phase 1 with the
    ///                     tampered register and emit only when the sink flips MISS→HIT *and* the
    ///                     packet disposition changes (emit hit_phase=3 / miss_phase=1 + case label
    ///                     + phase3_verify).
    /// Returns the number of sub-tests emitted for this (chain, direction).
    size_t runTamperingChain(const P4StateDependency::DependencyGraphs::SOChain &chain,
                             const ExecutionState &initState, const TamperingCallback &callBack,
                             size_t maxPerChain, bool missToHit);

    /// Extracts the concrete (input, output) port pair from a final state's model.
    std::pair<int, int> getPortPair(const FinalState *fs) const;

    /// Symbolically replays @p fs1's input packet with the registers in @p carriedRegs pre-set
    /// (the tampered state), pinning the packet bytes, size, and input port. Used by both tamper
    /// directions to compute the actual Phase-3 disposition. Returns the single Phase-3 terminal,
    /// or nullptr if the pinned input has no satisfiable terminal.
    const FinalState *runSymbolicPhase3(
        const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
        const FinalState *fs1, int inputPort, const IR::Expression *inputPortSymExpr,
        const std::map<cstring, const TestObject *> &carriedRegs);

    /// Evaluates the current sink table's hit-var in @p fs's final model.
    /// Returns 1 (HIT), 0 (MISS / not reached), or -1 (unknown: no sink, or tainted/non-literal).
    int evalSinkHit(const FinalState *fs) const;

    /// Evaluates @p fs's packet disposition from its final model: sets @p dropped (packet not
    /// emitted: drop property, empty buffer, or tainted egress port) and, when not dropped,
    /// @p outPort to the concrete egress port (else -1).
    void evalDisposition(const FinalState *fs, bool &dropped, int &outPort) const;

    /// Evaluates the target's multicast-group metadata in @p fs. Returns the concrete non-zero
    /// multicast group id if the packet is multicast-forwarded, or -1 if no group is set / the
    /// target does not model multicast / the value is tainted.
    int evalMulticastGroup(const FinalState *fs) const;

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
