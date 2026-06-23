#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_

#include <functional>
#include <map>
#include <optional>
#include <set>
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
    Tampering,      ///< H2S2K: register → table KEY (sink-table HIT/MISS flip).
    TamperingCond,  ///< H2S2C: register → if CONDITION (true/false flip).
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

    /// Number of times the Phase-2 (attacker) packet must be replayed for the tamper to take
    /// effect. 1 = today's single packet. >1 = accumulation: the same Phase-2 packet is sent k
    /// times so a register increment crosses the threshold that flips the downstream condition /
    /// sink. The count is data-driven (symbex replays + Phase-3 flip check until it flips, bounded
    /// by --max-phase2-packets). The serializer expands this into k marked input_packet blocks.
    size_t phase2RepeatCount = 1;

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

    /// The current chain's sink if-condition (H2S2C / TamperingCond policy), or nullptr for Key
    /// chains. Set per chain in runTamperingChain from SOChain::sinkConditionNode. Exactly one of
    /// currentSinkTable_ / currentSinkCondition is non-null for a resolvable sink.
    const IR::IfStatement *currentSinkCondition = nullptr;

    /// True while runMissToHitChain collects Phase-1 sink-MISS baselines. Disables pickSuccessor's
    /// sink-HIT steering (which would otherwise pull Phase 1 onto HIT branches we discard).
    bool seekMiss_ = false;

    /// True while driveRegisterPhase2 searches for a "priming" packet. Relaxes the Phase-2 terminal
    /// acceptance from full allCovered coverage to "wrote the SO register" so a single increment
    /// packet (which can't reach the branch-gated write path in one shot) is still captured to
    /// measure delta. Never set during emission of an actual test.
    bool phase2AcceptWroteSO_ = false;

    // ---- Shared Phase-1 collection (one traversal for all chains) -----------------------------
    /// True while collectPhase1Terminals runs the single shared Phase-1 DFS. In this mode runImpl
    /// buckets each terminal into every chain it covers (instead of the single-chain allCovered),
    /// and pickSuccessor steers/prunes toward the UNION of all chains' read targets.
    bool sharedPhase1 = false;
    /// All SOChains for the current program (flattened from collectChains), used by the shared pass.
    std::vector<const P4StateDependency::DependencyGraphs::SOChain *> allChains;
    /// Per-chain Phase-1 target node set (readNodes, or writeNodes for isUpdate chains), keyed by
    /// chain id. Used to test coverage of a terminal against each chain.
    std::map<size_t, P4::Coverage::CoverageSet> chainPhase1Targets;
    /// Collected Phase-1 terminals per chain id (a terminal may appear in several chains' buckets).
    std::map<size_t, std::vector<const FinalState *>> phase1Buckets;
    /// Per-chain cap on collected Phase-1 terminals (covers both directions' post-filter needs).
    size_t phase1BucketCap = 0;
    /// Safety bound on terminals examined by the shared pass (stops runaway exploration when some
    /// chain is single-packet-infeasible and its bucket never fills).
    size_t phase1ExamineBudget = 0;
    size_t phase1Examined = 0;

    /// Runs the single shared Phase-1 DFS, filling phase1Buckets for every chain in allChains.
    void collectPhase1Terminals(const ExecutionState &initState);
    /// Shared-pass terminal handler: bucket @p es into every chain whose Phase-1 targets it covers.
    void handleSharedTerminal(const ExecutionState &es);
    /// True when chain @p chain's Phase-1 target nodes are all covered in @p visited. A target node
    /// that lies inside a RegisterAction body is EXCUSED when @p soRan (the SO's RegisterAction was
    /// executed): its read path crosses mutually-exclusive if-branches that no single path can
    /// jointly cover. Nodes OUTSIDE the RegisterAction (incl. the .execute() call site and the sink
    /// key) stay strictly required, so this only relaxes the in-RegisterAction blocks.
    bool chainTargetsCovered(const P4StateDependency::DependencyGraphs::SOChain &chain,
                             const P4::Coverage::CoverageSet &visited, bool soRan) const;
    /// True when @p chain's sink if-condition was reached on @p es's path (its branch-stamped
    /// condition var is set). The Phase-1 baseline criterion for H2S2C chains (whose readNodes are
    /// empty for read-modify-write SOs, so node-coverage is unusable).
    bool conditionReached(const P4StateDependency::DependencyGraphs::SOChain &chain,
                          const ExecutionState &es) const;
    /// True when every chain's Phase-1 bucket has reached phase1BucketCap.
    bool allPhase1BucketsFull() const;

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
    /// Returns the number of sub-tests emitted for this (chain, direction). @p phase1Bucket is the
    /// chain's share of the shared Phase-1 traversal (collectPhase1Terminals); this function applies
    /// the direction-specific filter to it and runs Phase 2 / Phase 3.
    size_t runTamperingChain(const P4StateDependency::DependencyGraphs::SOChain &chain,
                             const ExecutionState &initState,
                             const std::vector<const FinalState *> &phase1Bucket,
                             const TamperingCallback &callBack, size_t maxPerChain, bool missToHit);

    /// Extracts the concrete (input, output) port pair from a final state's model.
    std::pair<int, int> getPortPair(const FinalState *fs) const;

    /// Symbolically replays @p fs1's input packet with the registers in @p carriedRegs pre-set
    /// (the tampered state), pinning the packet bytes, size, and input port. Used by both tamper
    /// directions to compute the actual Phase-3 disposition. Returns the single Phase-3 terminal,
    /// or nullptr if the pinned input has no satisfiable terminal.
    /// @param keepWriteCoverage when true, run the pinned replay under the Phase-2 write-coverage
    /// criterion (currentRequiredNodes = writeNodes + allCovered acceptance) instead of the default
    /// "accept any terminal" condition-replay. Used by the analytical drive-register path to
    /// re-validate that the full (branch-gated) write path is covered once the register is pre-set
    /// past the gate.
    const FinalState *runSymbolicPhase3(
        const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
        const FinalState *fs1, int inputPort, const IR::Expression *inputPortSymExpr,
        const std::map<cstring, const TestObject *> &carriedRegs, bool keepWriteCoverage = false);

    /// Pins @p init's input packet (every pktvar_N), packet size, and input port to @p fs1's model
    /// values, so a cloned execution replays fs1's exact packet/flow. Used by runSymbolicPhase3 (to
    /// replay Phase 1 in Phase 3) and as the whole-packet fallback for a RANDOM-hash (tainted) index.
    void pinPacketToPhase1(ExecutionState &init, const FinalState *fs1, int inputPort,
                           const IR::Expression *inputPortSymExpr);

    /// Fills @p init's input packet AND parser buffer with @p fs's concrete packet bytes (and pins
    /// size + input port), so the parser slices constants — every CRC Hash.get then resolves eagerly
    /// during execution and CRC-indexed register accesses land in the real cell. Used to replay a
    /// terminal with a determined (fork-free) hash path; @see reDeriveConcretePhase / runSymbolicPhase3.
    void concretizeInputPacket(ExecutionState &init, const FinalState *fs, int inputPort,
                               const IR::Expression *inputPortSymExpr);

    /// Re-derives an emitted phase terminal whose hash inputs are CONCRETE. Pre-fills the input
    /// packet AND the parser buffer with @p fs's concrete packet bytes (from its model), so the
    /// parser slices constants (no fresh pktvars) — every CRC hash then resolves eagerly during
    /// execution and the path follows the REAL hash branch, fork-free. The resulting terminal is
    /// hash-consistent, so TestBackEnd::processPhase re-solves it SAT (an arbitrary-fork shared
    /// terminal does not). Returns the terminal matching @p fs's disposition; falls back to @p fs if
    /// none matches (so this is strictly additive — it can only fix the 0-emit case).
    /// @p carriedRegs / @p sinkTableName mirror runSymbolicPhase3's Phase-2 setup (empty / "" for the
    /// no-tamper Phase-1 reference).
    const FinalState *reDeriveConcretePhase(
        const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
        const FinalState *fs, int inputPort, const IR::Expression *inputPortSymExpr, bool isPhase1,
        const std::map<cstring, const TestObject *> &carriedRegs);

    /// Collects the packet-field SymbolicVariable leaves of @p soReg's index expression(s) — the
    /// index-determining inputs (e.g. the operands of a concolic CRC hash). Empty when the index is a
    /// constant or an opaque TaintExpression (RANDOM hash).
    [[nodiscard]] std::set<const IR::SymbolicVariable *> collectIndexSymVars(
        const TestObject *soReg) const;

    /// Pins exactly @p symVars in @p init to their @p fs1 model values (index-input pinning) — the
    /// attacker packet then hashes to the Phase-1 flow's bucket while leaving non-index fields free.
    void pinIndexInputsToPhase1(ExecutionState &init, const FinalState *fs1,
                                const std::set<const IR::SymbolicVariable *> &symVars);

    /// Accumulation driver: starting from @p fs2 (a reachable Phase-2 write packet), replay the SAME
    /// packet — carrying the register state forward each time — and re-run symbolic Phase 3 until the
    /// sink/condition flips to @p p3Target (evalSinkFlip). Returns the flipped Phase-3 terminal and
    /// sets @p outRepeat to the number of Phase-2 packet sends (k); returns nullptr if no count up to
    /// --max-phase2-packets flips it (or a fixpoint is reached: the carried SO value stops changing).
    /// k=1 reproduces today's single-packet behavior. Reuses runSymbolicPhase3 as the pin+carry+run
    /// engine for both the packet replay and the Phase-3 flip check.
    const FinalState *accumulatePhase2Flip(
        const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
        const FinalState *fs1, int inputPort1, const FinalState *fs2, int inputPort2,
        const IR::Expression *inputPortSymExpr, int p3Target, size_t &outRepeat);

    /// Analytical drive-register path (Family 1: the action runs but a register-VALUE gate on the
    /// write path is unreached in one packet, so the single-packet allCovered DFS finds nothing).
    /// Finds a priming packet (a terminal that wrote the SO, accepted via the relaxed "wrote-SO"
    /// criterion), extracts (init, delta) and the threshold (op, C) from the missed gated
    /// IfStatement, computes how many sends k drive the register past the gate, pre-sets the carried
    /// SO to that value and RE-RUNS the real allCovered DFS (keepWriteCoverage) to validate that the
    /// full write path is now covered, then runs Phase 3 for the flip+diverge check. On success
    /// returns the flipped Phase-3 terminal, sets @p outFs2 to the validated covering Phase-2 write
    /// terminal and @p outRepeat to k; returns nullptr (sound skip) on any failure/fallback.
    /// @p phase2Init is a fresh clone already carrying Phase-1 registers + port/key constraints.
    const FinalState *driveRegisterPhase2(
        const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
        ExecutionState &phase2Init, const FinalState *fs1, int inputPort1,
        const IR::Expression *inputPortSymExpr, int p3Target, const FinalState *&outFs2,
        size_t &outRepeat);

    /// True when @p es's final register state recorded a write to the current chain's SO register.
    /// The relaxed Phase-2 acceptance used to capture a priming packet (vs full allCovered coverage).
    bool terminalWroteSO(const ExecutionState &es) const;

    /// Evaluates the current sink table's hit-var in @p fs's final model.
    /// Returns 1 (HIT), 0 (MISS / not reached), or -1 (unknown: no sink, or tainted/non-literal).
    int evalSinkHit(const FinalState *fs) const;

    /// H2S2C analog of evalSinkHit: evaluates currentSinkCondition's condition in @p fs's final
    /// model. Returns 1 (condition true), 0 (false), or -1 (not reached / no condition / tainted).
    int evalCondition(const FinalState *fs) const;

    /// Direction-agnostic sink-flip value: dispatches to evalSinkHit (Key sink) or evalCondition
    /// (condition sink). Returns 1 (HIT / true), 0 (MISS / false), -1 (unreached / unknown).
    int evalSinkFlip(const FinalState *fs) const;

    /// Condition action-divergence gate (H2S2C analog of sinkActionsDiverge): true unless
    /// currentSinkCondition's then-branch (ifTrue) and else-branch (ifFalse) are provably identical
    /// in observable effect — i.e. flipping the condition changes nothing. Null condition ⇒ true.
    bool sinkConditionDiverges() const;

    /// Runs the H2S2C condition-chain flow for one (chain, direction): filters @p phase1Bucket by the
    /// Phase-1 condition value, reuses the Phase-2 write DFS, then symbolically replays Phase 3 and
    /// emits when the condition flips and the branches diverge. Returns sub-tests emitted.
    size_t runConditionChain(const P4StateDependency::DependencyGraphs::SOChain &chain,
                             const ExecutionState &initState,
                             const std::vector<const FinalState *> &phase1Bucket,
                             const TamperingCallback &callBack, size_t maxPerChain, bool missToHit);

    /// Evaluates @p fs's packet disposition from its final model: sets @p dropped (packet not
    /// emitted: drop property, empty buffer, or tainted egress port) and, when not dropped,
    /// @p outPort to the concrete egress port (else -1).
    void evalDisposition(const FinalState *fs, bool &dropped, int &outPort) const;

    /// Sink action-divergence gate (replaces the cross-phase Phase1↔Phase3 output comparison).
    /// Returns true unless the sink table's HIT action (the action of @p fs's matched entry) and its
    /// default (MISS) action are provably identical in observable effect — i.e. a HIT↔MISS flip
    /// would change nothing. @p sink may be nullptr (returns true). Compares the two action bodies
    /// *locally* — no continuation, no terminal, no downstream result consulted; the real switch
    /// judges end-to-end. Sound-toward-emitting: anything not provably identical counts as divergent.
    bool sinkActionsDiverge(const FinalState *fs, const IR::P4Table *sink, cstring sinkCpName) const;

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

    /// Source positions of every node inside a RegisterAction body (its abstract `apply` method).
    /// Keyed by position (not pointer) because the chain's required nodes come from the SD dependency
    /// graph's IR, whose pointers differ from programInfo.getP4Program()'s — same reason getConditionVar
    /// is position-keyed. Used to scope the read/write coverage relaxation to in-RegisterAction blocks
    /// only: such a node may be an unreachable mutually-exclusive branch sibling, so it is excused once
    /// the RegisterAction ran; nodes outside the RegisterAction stay strictly required.
    std::unordered_set<cstring> registerActionBodyPositions_;

    /// True if @p n's source position is inside a RegisterAction body.
    bool isInRegisterActionBody(const IR::Node *n) const;

    /// Populates registerActionBodyPositions_ by traversing the P4 program once.
    void buildRegisterActionBodyNodes();

    /// Returns true if the table with the given control-plane name was visited.
    bool isTableVisited(cstring controlPlaneName,
                        const P4::Coverage::CoverageSet &visited) const;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_ */
