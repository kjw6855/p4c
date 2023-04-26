#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_

#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "backends/p4tools/modules/fuzzer/core/small_step/small_step.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/lib/execution_state.h"
#include "backends/p4tools/modules/fuzzer/p4testgen.pb.h"

using ExecutionState = P4Tools::P4Testgen::ExecutionState;
using p4testgen::TestCase;

namespace P4Tools {

namespace P4Testgen {

/// Explores one path described by a list of branches.
class SelectedTest {
 public:
    /// Executes the P4 program along a randomly chosen path. When the program terminates, the
    /// given callback is invoked. If the callback returns true, then the executor terminates.
    /// Otherwise, execution of the P4 program continues on a different random path.
    void run(const TestCase& testCase);

    /// Constructor for this strategy, considering inheritance
    SelectedTest(const ProgramInfo& programInfo);

    const P4::Coverage::CoverageSet& getVisitedStatements();

    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

 protected:
    /// Target-specific information about the P4 program.
    const ProgramInfo& programInfo;

    /// Chooses a branch corresponding to a given branch identifier.
    ///
    /// @returns next execution state to be examined, throws an exception on invalid nextBranch.
    ExecutionState* chooseBranch(const std::vector<Branch>& branches, uint64_t nextBranch);

    bool testHandleTerminalState(const ExecutionState& terminalState);

    /// Take one step in the program and return list of possible branches.
    Result step(ExecutionState& state, const TestCase& testCase);

    /// The current execution state.
    ExecutionState* executionState = nullptr;

    /// Set of all stetements, to be retrieved from programInfo.
    const P4::Coverage::CoverageSet& allStatements;

    /// Set of all statements executed in any testcase that has been outputted.
    P4::Coverage::CoverageSet visitedStatements;

 private:
    /// Reachability engine.
    ReachabilityEngine* reachabilityEngine = nullptr;

    /// The number of times a guard was not satisfiable.
    uint64_t violatedGuardConditions = 0;
};

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_EXPLORATION_STRATEGY_SELECTED_TEST_H_ */
