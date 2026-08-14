#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_BACKEND_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_BACKEND_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/final_state.h"
#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

class TestBackEnd {
 private:
    /// The current test count. If it exceeds @var maxTests, the symbolic executor will stop.
    int64_t testCount = 0;

    /// Indicates the number of generated tests after which we reset memory.
    static const int64_t RESET_THRESHOLD = 10000;

    /// ProgramInfo is used to access some target specific information for test generation.
    std::reference_wrapper<const ProgramInfo> programInfo;

    /// Configuration options for the test back end.
    std::reference_wrapper<const TestBackendConfiguration> testBackendConfiguration;

 protected:
    /// Writes the tests out to a file.
    TestFramework *testWriter = nullptr;

    /// Pointer to the symbolic executor.
    /// TODO: Remove this. We only need to update coverage tracking.
    SymbolicExecutor &symbex;

    /// Test maximum number of tests that are to be produced.
    int64_t maxTests;

    /// The accumulated coverage of all finished test cases. Number in range [0, 1].
    float coverage = 0;

    /// The list of tests accumulated in the test back end.
    AbstractTestList tests;

    explicit TestBackEnd(const ProgramInfo &programInfo,
                         const TestBackendConfiguration &testBackendConfiguration,
                         SymbolicExecutor &symbex);

    [[nodiscard]] bool needsToTerminate(int64_t testCount) const;

 public:
    TestBackEnd(const TestBackEnd &) = default;

    TestBackEnd(TestBackEnd &&) = default;

    TestBackEnd &operator=(const TestBackEnd &) = delete;

    TestBackEnd &operator=(TestBackEnd &&) = delete;

    virtual ~TestBackEnd() = default;

    struct TestInfo {
        /// The concrete value of the input packet.
        /// This is a slice of the program packet according to packetSizeInInt.
        const IR::Constant *inputPacket;

        /// The input port of the packet.
        int inputPort;

        /// The concrete value of the output packet as modified by the packet.
        const IR::Constant *outputPacket;

        /// The output port of the packet.
        int outputPort;

        /// The taint mask.
        const IR::Constant *packetTaintMask;

        /// The traces that have been collected during execution of this particular test path.
        const std::vector<std::reference_wrapper<const TraceEvent>> programTraces;

        /// Indicates whether the packet is dropped.
        bool packetIsDropped = false;

        /// True when the egress port is tainted but NOT a genuine drop — i.e. symbex could not pin
        /// the port (e.g. a hash-derived egress). The port is then emitted as the reserved sentinel
        /// SYMBEX_UNKNOWN_PORT (-1) meaning "unknown, decided at replay"; the differential oracle
        /// observes the actual egress. Distinct from packetIsDropped (UninitializedTaintExpression /
        /// egress_spec==DROP_PORT), which is a real drop.
        bool outputPortIsUnknown = false;
    };

    /// Reserved output-port value meaning "symbex could not determine the egress port" (tainted,
    /// non-drop). The tampering harness ignores it and observes the actual egress at replay.
    static constexpr int SYMBEX_UNKNOWN_PORT = -1;

    /// Result of processing a single phase (one packet execution) without writing output.
    struct PhaseResult {
        /// The created test specification for this phase.
        const TestSpec *testSpec;
        /// Whether the packet was dropped during this phase.
        bool packetIsDropped;
        /// True when this phase's register index was reported but not enforced: asserting the real
        /// hash value contradicted the path (a program branching on hash bits has already committed
        /// to an arbitrary cell), so the emitted index describes the model rather than constraining
        /// it. Surfaced in the emitted metadata so triage can tell the two kinds of case apart.
        bool indexReportedNotEnforced = false;
    };

    /// Resolves concolic variables, produces TestInfo, and creates a TestSpec for one phase
    /// without writing any output or incrementing the test counter.
    /// Returns std::nullopt if the phase should be skipped (e.g. tainted output port,
    /// failed concolic resolution).
    ///
    /// When overrideInputPort / overrideOutputPort are set, those concrete values replace
    /// whatever the re-solved model evaluates for the port variables. This is necessary for
    /// the tampering scenario because computeConcolicState() re-solves and may pick different
    /// (but equally valid) concrete port values than the ones computed in runTamperingScenario.
    ///
    /// modelOverrides are (SymbolicVariable → Constant) pairs applied via Model::set() AFTER
    /// computeConcolicState(). For Phase 2, these inject attacker-chosen register values into
    /// the model so the emitted input packet shows the tampered field value.
    /// @param allowTaintedOutput when true, do not reject a state whose output port is tainted
    /// (unknown). Used by the tampering differential oracle: the real switch decides the egress at
    /// replay and the harness compares the two runs' actual Phase-3 outputs, so a symbex-unknown
    /// output port is fine. The output packet is emitted with its taint mask (don't-care bytes).
    [[nodiscard]] virtual std::optional<PhaseResult> processPhase(
        const FinalState &state,
        std::optional<int> overrideInputPort = std::nullopt,
        std::optional<int> overrideOutputPort = std::nullopt,
        const std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>>
            &modelOverrides = {},
        const std::vector<const IR::Expression *> &extraConstraints = {},
        bool allowTaintedOutput = false);

    /// @returns the test specification which is consumed by the test back ends.
    virtual const TestSpec *createTestSpec(const ExecutionState *executionState,
                                           const Model *finalModel, const TestInfo &testInfo) = 0;

    /// Prints information about this particular test path.
    /// @returns false if the test generation is to be aborted (for example when the port is
    /// tainted.)
    virtual bool printTestInfo(const ExecutionState *executionState, const TestInfo &testInfo,
                               const IR::Expression *outputPortExpr);

    /// @returns a new modules with all concolic variables in the program resolved.
    [[nodiscard]] std::optional<std::reference_wrapper<const FinalState>> computeConcolicVariables(
        const FinalState &state) const;

    /// @returns a TestInfo objects, which contains information about the input/output ports, the
    /// taint mask, the packet sizes, etc...
    virtual TestInfo produceTestInfo(
        const ExecutionState *executionState, const Model *finalModel,
        const IR::Expression *outputPacketExpr, const IR::Expression *outputPortExpr,
        const std::vector<std::reference_wrapper<const TraceEvent>> *programTraces);

    /// The callback that is executed by the symbolic executor.
    virtual bool run(const FinalState &state);

    /// Callback for the three-phase tampering scenario.
    /// The default implementation emits three independent test cases by calling run() for
    /// each phase; target backends can override this to emit a single TamperingTestSpec
    /// that bundles all three packet pairs with one shared set of table entries.
    virtual bool runTampering(const TamperingFinalState &state);

    /// Returns test count.
    [[nodiscard]] int64_t getTestCount() const;

    /// Returns coverage achieved by all the processed tests.
    [[nodiscard]] float getCoverage() const;

    /// Returns the program info.
    [[nodiscard]] const ProgramInfo &getProgramInfo() const;

    /// Returns the configuration options for the test back end.
    [[nodiscard]] const TestBackendConfiguration &getTestBackendConfiguration() const;

    /// Returns the list of tests accumulated in the test back end.
    /// If the test write is in file mode this list will be empty.
    [[nodiscard]] const AbstractTestList &getTests() const { return tests; }
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_BACKEND_H_ */
