#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_

#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "backends/p4tools/modules/fuzzer/core/small_step/visit_step_evaluator.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/visit_stepper.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_concolic.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/lib/final_visit_state.h"
#include "backends/p4tools/modules/fuzzer/p4testgen.pb.h"

using p4testgen::TestCase;

namespace P4Tools {

namespace P4Testgen {

/// Explores one path described by a list of branches.
class SelectedTest {
 public:
    ~SelectedTest();

    /// Constructor for this strategy, considering inheritance
    SelectedTest(const ProgramInfo& programInfo);

    /// Executes the P4 program along a randomly chosen path. When the program terminates, the
    /// given callback is invoked. If the callback returns true, then the executor terminates.
    /// Otherwise, execution of the P4 program continues on a different random path.
    void run(const TestCase& testCase);

    const P4::Coverage::CoverageSet& getVisitedStatements();

    const std::string getStatementBitmapStr();

    boost::optional<Packet> getOutputPacket();

    using VisitBranch = VisitStepEvaluator::VisitBranch;
    using VisitResult = VisitStepEvaluator::VisitResult;

 protected:
    /// Target-specific information about the P4 program.
    const ProgramInfo& programInfo;

    /// Chooses a branch corresponding to a given branch identifier.
    ///
    /// @returns next execution state to be examined, throws an exception on invalid nextVisitBranch.
    VisitState* chooseVisitBranch(const std::vector<VisitBranch>& branches, uint64_t nextVisitBranch);

    bool testHandleTerminalState(const VisitState& terminalState);

    /// The current execution state.
    VisitState* executionState = nullptr;

    FinalVisitState* finalState = nullptr;

    /// Set of all stetements, to be retrieved from programInfo.
    const P4::Coverage::CoverageSet& allStatements;

    /// Set of all statements executed in any testcase that has been outputted.
    P4::Coverage::CoverageSet visitedStatements;

 public:
    const int statementBitmapSize;
    unsigned char* statementBitmap;

 private:
    VisitStepEvaluator evaluator;
};

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_ */
