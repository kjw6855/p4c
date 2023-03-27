#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/selected_test.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "backends/p4tools/common/core/solver.h"
#include "gsl/gsl-lite.hpp"
#include "lib/error.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/exploration_strategy.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/abstract_stepper.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/small_step.h"

namespace P4Tools {

namespace P4Testgen {

void SelectedTest::run(const Callback& callback) {
    while (!executionState->isTerminal()) {

        StepResult successors = step(*executionState);

        if (const auto cmdOpt = executionState->getNextCmd()) {
            struct CmdLogger : public boost::static_visitor<> {
             private:
                SelectedTest& self;
                StepResult& result;

             public:
                void operator()(const IR::Node* node) {

                    std::cout << "[" << node->node_type_name() << "]";
                    if (dynamic_cast<const IR::P4Program*>(node) == nullptr &&
                            dynamic_cast<const IR::P4Control*>(node) == nullptr) {
                        std::cout << " " << node;
                    }


                    if (result->size() > 1) {
                        std::cout << " (" << result->size() << " branches)";
                    }

                    std::cout << std::endl;
                }
                void operator()(const TraceEvent* event) {}
                void operator()(Continuation::Return ret) {}
                void operator()(Continuation::Exception e) {}
                void operator()(const Continuation::PropertyUpdate& e) {}
                void operator()(const Continuation::Guard& guard) {}

                explicit CmdLogger(SelectedTest& self, StepResult& result)
                    : self(self), result(result) {}
            } cmdLogger(*this, successors);

            boost::apply_visitor(cmdLogger, *cmdOpt);
        }

        if (successors->size() == 1) {
            // Non-branching states are not recorded by selected branches.
            executionState = (*successors)[0].nextState;
            continue;
        }
        // If there are multiple, pop one branch decision from the input list and pick
        // successor matching the given branch decision.
        ExecutionState* next = chooseBranch(*successors, 0);
        if (next == nullptr) {
            break;
        }
        executionState = next;
    }
    if (executionState->isTerminal()) {
        // We've reached the end of the program. Call back and (if desired) end execution.
        testHandleTerminalState(*executionState);
        return;
    }
}

/*
uint64_t getNumeric(const std::string& str) {
    char* leftString = nullptr;
    uint64_t number = strtoul(str.c_str(), &leftString, 10);
    BUG_CHECK(!(*leftString), "Can't translate selected branch %1% into int", str);
    return number;
}
*/

SelectedTest::SelectedTest(AbstractSolver& solver, const ProgramInfo& programInfo,
        boost::optional<uint32_t> seed)
    : ExplorationStrategy(solver, programInfo, seed) {
    /*
    auto str = std::move(selectedBranchesStr);
    while ((n = str.find(',')) != std::string::npos) {
        selectedBranches.push_back(getNumeric(str.substr(0, n)));
        str = str.substr(n + 1);
    }
    if (str.length() != 0U) {
        selectedBranches.push_back(getNumeric(str));
    }
    */
}

ExecutionState* SelectedTest::chooseBranch(const std::vector<Branch>& branches,
                                               uint64_t nextBranch) {
    ExecutionState* next = nullptr;
    for (const auto& branch : branches) {
        const auto& selectedBranches = branch.nextState->getSelectedBranches();
        BUG_CHECK(!selectedBranches.empty(), "Corrupted selectedBranches in a execution state");
        // Find branch matching given branch identifier.
        if (selectedBranches.back() == nextBranch) {
            next = branch.nextState;
            break;
        }

        if (next == nullptr) {
            next = branch.nextState;
        }
    }

    if (!next) {
        // If not found, the input selected branch list is invalid.
        ::error("The selected branches string doesn't match any branch.");
    }

    return next;
}

bool SelectedTest::testHandleTerminalState(const ExecutionState& terminalState) {

    std::cout << std::endl << "[FINAL]" << std::endl;
    for (const auto& stmt : terminalState.getVisited()) {
        if (allStatements.count(stmt) != 0U) {
            visitedStatements.insert(stmt);

            std::cout << "[" << stmt->node_type_name() << "] " << stmt << std::endl;
        }
    }

    //const FinalState finalState(&solver, terminalState);
    //return callback(finalState);

    return true;
}

}  // namespace P4Testgen

}  // namespace P4Tools
