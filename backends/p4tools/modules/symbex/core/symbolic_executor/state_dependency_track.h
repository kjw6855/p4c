#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_

#include <functional>
#include <map>
#include <optional>
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

/// Bundles the three terminal FinalStates that together form one tampering test case:
///   phase1 — packet that reads the original register value
///   phase2 — packet that writes the tampered value
///   phase3 — packet that reads the tampered value (register pre-initialized from phase2)
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
    const FinalState &phase3;
    bool readPathHasExit = false;
    int phase1InputPort = -1;
    int phase1OutputPort = -1;
    int phase2InputPort = -1;
    int phase2OutputPort = -1;
    int phase3InputPort = -1;
    int phase3OutputPort = -1;
    /// Attacker-chosen register values (random by default) generated during Phase 3 seeding.
    /// Maps register control-plane name → evaluated TestObject with random concrete values.
    /// Populated by runTamperingScenario(); available to test writers for consistent output.
    std::map<cstring, const TestObject *> attackerRegisterValues;
    /// Direct model overrides for Phase 2: each (SymbolicVariable, Constant) pair is applied
    /// via Model::set() after computeConcolicState() so that the emitted Phase 2 input packet
    /// shows the attacker-chosen value in the field that is written to the register.
    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>> phase2ModelOverrides;
};

/// Callback type for the three-phase tampering scenario.
using TamperingCallback = std::function<bool(const TamperingFinalState &)>;

/// Symbolic executor that steers path selection toward SOChains from state-dependency
/// analysis. For each SOChain the executor runs an independent DFS from the initial
/// program state, preferring branches that cover the chain's required IR nodes. A test
/// vector is emitted only when a terminal path covers ALL required nodes.
///
/// Policy variants map to different chain sources and node sets:
///   - Tampering    : dataWriteValueChains + dataWriteCondChains
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

    /// Entry point. For Tampering, runs three phases per chain and invokes the outer
    /// callback once per (phase1, phase2, phase3) triple via runTampering().
    /// For other policies, runs a single DFS per chain as before.
    void run(const Callback &callBack) override;

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

    /// The SOChain being explored (set in run(), read in runImpl()).
    const P4StateDependency::DependencyGraphs::SOChain *currentChain = nullptr;

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
    /// constraints already pushed), saving each terminal FinalState into out.
    /// Stops after the first accepted result.
    void runPhase(ExecutionState &phaseInit, std::vector<const FinalState *> &out);

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
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_STATE_DEPENDENCY_TRACK_H_ */
