#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/selected_test.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "backends/p4tools/common/lib/timer.h"
#include "gsl/gsl-lite.hpp"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/node.h"
#include "lib/error.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/exploration_strategy.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/lib/execution_state.h"

namespace P4Tools {

namespace P4Testgen {

void SelectedTest::run(const TestCase& testCase) {
    while (!executionState->isTerminal()) {

        Result successors = step(*executionState, testCase);

        if (const auto cmdOpt = executionState->getNextCmd()) {
            struct CmdLogger : public boost::static_visitor<> {
             private:
                SelectedTest& self;
                Result& result;

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

                explicit CmdLogger(SelectedTest& self, Result& result)
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

SelectedTest::Result SelectedTest::step(ExecutionState& state, const TestCase& testCase) {
    ScopedTimer st("step");

    if (const auto cmdOpt = state.getNextCmd()) {
        struct CommandVisitor : public boost::static_visitor<Result> {
         private:
            SelectedTest& self;
            const TestCase& testCase;
            ExecutionState& state;

         public:
            using REngineType = std::pair<ReachabilityResult, std::vector<Branch>*>;
            REngineType renginePreprocessing(const IR::Node* node) {
                ReachabilityResult rresult = std::make_pair(true, nullptr);
                std::vector<Branch>* branches = nullptr;
                // Current node should be inside DCG.
                if (self.reachabilityEngine->getDCG()->isCaller(node)) {
                    // Move reachability engine to next state.
                    rresult = self.reachabilityEngine->next(state.reachabilityEngineState, node);
                    if (!rresult.first) {
                        // Reachability property was failed.
                        const IR::Expression* cond = IR::getBoolLiteral(false);
                        branches = new std::vector<Branch>({Branch(cond, state, &state)});
                    }
                } else if (const auto* method = node->to<IR::MethodCallStatement>()) {
                    return renginePreprocessing(method->methodCall);
                }
                return std::make_pair(rresult, branches);
            }

            static void renginePostprocessing(ReachabilityResult& result,
                                              std::vector<Branch>* branches) {
                // All Reachability engine state for branch should be copied.
                if (branches->size() > 1 || result.second != nullptr) {
                    for (auto& n : *branches) {
                        if (result.second != nullptr) {
                            n.constraint =
                                new IR::BAnd(IR::Type_Boolean::get(), n.constraint, result.second);
                        }
                        if (branches->size() > 1) {
                            // Copy reachability engine state
                            n.nextState->reachabilityEngineState =
                                n.nextState->reachabilityEngineState->copy();
                        }
                    }
                }
            }

            Result operator()(const IR::Node* node) {
                // Step on the given node as a command.
                BUG_CHECK(node, "Attempted to evaluate null node.");
                REngineType r;
                if (self.reachabilityEngine != nullptr) {
                    r = renginePreprocessing(node);
                    if (r.second != nullptr) {
                        return r.second;
                    }
                }

                /*****************
                 * TODO
                 *****************/
                /*
                auto* stepper = TestgenTarget::getCmdStepper(state, self.solver, self.programInfo);
                auto* result = stepper->step(node);
                if (self.reachabilityEngine != nullptr) {
                    renginePostprocessing(r.first, result);
                }
                return result;
                */
                return new std::vector<Branch>({Branch(&state)});
            }

            Result operator()(const TraceEvent* event) {
                CHECK_NULL(event);
                event = event->subst(state.getSymbolicEnv());

                state.add(event);
                state.popBody();
                return new std::vector<Branch>({Branch(&state)});
            }

            Result operator()(Continuation::Return ret) {
                if (ret.expr) {
                    // Step on the returned expression.
                    const auto* expr = *ret.expr;
                    BUG_CHECK(expr, "Attempted to evaluate null expr.");

                    // Do not bother with the stepper, if the expression is already symbolic.
                    if (SymbolicEnv::isSymbolicValue(expr)) {
                        state.popContinuation(expr);
                        return new std::vector<Branch>({Branch(&state)});
                    }
                    /*****************
                     * TODO
                     *****************/
                    /*
                    auto* stepper =
                        TestgenTarget::getExprStepper(state, self.solver, self.programInfo);
                    auto* result = stepper->step(expr);
                    if (self.reachabilityEngine != nullptr) {
                        ReachabilityResult rresult = std::make_pair(true, nullptr);
                        renginePostprocessing(rresult, result);
                    }
                    return result;
                    */
                    return new std::vector<Branch>({Branch(&state)});
                }

                // Step on valueless return.
                state.popContinuation();
                return new std::vector<Branch>({Branch(&state)});
            }

            Result operator()(Continuation::Exception e) {
                state.handleException(e);
                return new std::vector<Branch>({Branch(&state)});
            }

            Result operator()(const Continuation::PropertyUpdate& e) {
                state.setProperty(e.propertyName, e.property);
                state.popBody();
                return new std::vector<Branch>({Branch(&state)});
            }

            Result operator()(const Continuation::Guard& guard) {
                // Check whether we exceed the number of maximum permitted guard violations.
                // This usually indicates that we have many branches that produce an invalid state.
                // The P4 program should be fixed in that case, because we can not generate useful
                // tests.
                if (self.violatedGuardConditions > SmallStepEvaluator::MAX_GUARD_VIOLATIONS) {
                    BUG("Condition %1% exceeded the maximum number of permitted guard "
                        "violations for this run."
                        " This implies that the P4 program produces an output that violates"
                        " test variants. For example, it may set an output port that is not "
                        "testable.",
                        guard.cond);
                }

                // Evaluate the guard condition by directly using the solver.
                const auto* cond = guard.cond;
                boost::optional<bool> solverResult = boost::none;

                // If the guard condition is tainted, treat it equivalent to an invalid state.
                /*****************
                 * TODO
                 *****************/
                /*
                if (!state.hasTaint(cond)) {
                    cond = state.getSymbolicEnv().subst(cond);
                    cond = P4::optimizeExpression(cond);
                    // Check whether the condition is satisfiable in the current execution state.
                    auto pathConstraints = state.getPathConstraint();
                    pathConstraints.push_back(cond);
                    solverResult = self.solver.checkSat(pathConstraints);
                }
                */

                auto* nextState = new ExecutionState(state);
                nextState->popBody();
                // If we can not solve the guard (either we time out or the solver can not solve the
                // problem) we increment the count of violatedGuardConditions and stop executing
                // this branch.
                if (solverResult == boost::none || !solverResult.get()) {
                    std::stringstream condStream;
                    guard.cond->dbprint(condStream);
                    ::warning(
                        "Guard %1% was not satisfiable."
                        " Incrementing number of guard violations.",
                        condStream.str().c_str());
                    self.violatedGuardConditions++;
                    return new std::vector<Branch>({{IR::getBoolLiteral(false), state, nextState}});
                }
                // Otherwise, we proceed as usual.
                return new std::vector<Branch>({{cond, state, nextState}});
            }

            explicit CommandVisitor(SelectedTest& self, const TestCase& testCase,
                    ExecutionState& state)
                : self(self), testCase(testCase), state(state) {}
        } cmdVisitor(*this, testCase, state);

        return boost::apply_visitor(cmdVisitor, *cmdOpt);
    }

    state.popContinuation();
    Result successors = new std::vector<Branch>({Branch(&state)});

    // Assign branch ids to the branches. These integer branch ids are used by track-branches
    // and selected (input) branches features.
    if (successors->size() > 1) {
        for (uint64_t bIdx = 0; bIdx < successors->size(); ++bIdx) {
            auto& succ = (*successors)[bIdx];
            succ.nextState->pushBranchDecision(bIdx + 1);
        }
    }

    return successors;
}

/*
uint64_t getNumeric(const std::string& str) {
    char* leftString = nullptr;
    uint64_t number = strtoul(str.c_str(), &leftString, 10);
    BUG_CHECK(!(*leftString), "Can't translate selected branch %1% into int", str);
    return number;
}
*/

SelectedTest::SelectedTest(const ProgramInfo& programInfo)
    : programInfo(programInfo),
      allStatements(programInfo.getAllStatements()) {

    executionState = new ExecutionState(programInfo.program);
    reachabilityEngine = new ReachabilityEngine(programInfo.dcg,
            std::string(), true);
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

const P4::Coverage::CoverageSet& SelectedTest::getVisitedStatements() {
    return visitedStatements;
}

}  // namespace P4Testgen

}  // namespace P4Tools
