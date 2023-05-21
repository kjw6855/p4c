#include "backends/p4tools/modules/fuzzer/core/small_step/visit_step_evaluator.h"

#include <iosfwd>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include "backends/p4tools/common/compiler/reachability.h"
#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/trace_events.h"
#include "frontends/p4/optimizeExpressions.h"
#include "gsl/gsl-lite.hpp"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/node.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/visit_stepper.h"
#include "backends/p4tools/modules/fuzzer/core/target.h"
#include "backends/p4tools/modules/fuzzer/lib/continuation.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/options.h"

namespace P4Tools {

namespace P4Testgen {

VisitStepEvaluator::VisitStepEvaluator(const ProgramInfo& programInfo)
    : programInfo(programInfo) {
}

VisitStepEvaluator::VisitBranch::VisitBranch(gsl::not_null<VisitState*> nextState)
    : constraint(IR::getBoolLiteral(true)), nextState(std::move(nextState)) {}

VisitStepEvaluator::VisitBranch::VisitBranch(boost::optional<const Constraint*> c,
                                    const VisitState& prevState,
                                    gsl::not_null<VisitState*> nextState)
    : constraint(IR::getBoolLiteral(true)), nextState(nextState) {
    if (c) {
        // Evaluate the branch constraint in the current state of symbolic environment.
        // Substitutes all variables to their symbolic value (expression on the program's initial
        // state).
        constraint = prevState.getSymbolicEnv().subst(*c);
        constraint = P4::optimizeExpression(constraint);
        // Append the evaluated and optimized constraint to the next execution state's list of
        // path constraints.
        nextState->pushPathConstraint(constraint);
    }
}

VisitStepEvaluator::VisitResult VisitStepEvaluator::step(VisitState& state, const TestCase& testCase) {
    //ScopedTimer st("step");

    if (const auto cmdOpt = state.getNextCmd()) {
        struct CommandVisitor : public boost::static_visitor<VisitResult> {
         private:
            VisitStepEvaluator& self;
            const TestCase& testCase;
            VisitState& state;

         public:
            using REngineType = std::pair<ReachabilityResult, std::vector<VisitBranch>*>;
            REngineType renginePreprocessing(const IR::Node* node) {
                ReachabilityResult rresult = std::make_pair(true, nullptr);
                std::vector<VisitBranch>* branches = nullptr;
                // Current node should be inside DCG.
                if (self.reachabilityEngine->getDCG()->isCaller(node)) {
                    // Move reachability engine to next state.
                    rresult = self.reachabilityEngine->next(state.reachabilityEngineState, node);
                    if (!rresult.first) {
                        // Reachability property was failed.
                        const IR::Expression* cond = IR::getBoolLiteral(false);
                        branches = new std::vector<VisitBranch>({VisitBranch(cond, state, &state)});
                    }
                } else if (const auto* method = node->to<IR::MethodCallStatement>()) {
                    return renginePreprocessing(method->methodCall);
                }
                return std::make_pair(rresult, branches);
            }

            static void renginePostprocessing(ReachabilityResult& result,
                                              std::vector<VisitBranch>* branches) {
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

            VisitResult operator()(const IR::Node* node) {
                // Step on the given node as a command.
                BUG_CHECK(node, "Attempted to evaluate null node.");
                REngineType r;
                if (self.reachabilityEngine != nullptr) {
                    r = renginePreprocessing(node);
                    if (r.second != nullptr) {
                        return r.second;
                    }
                }

                auto* stepper = TestgenTarget::getVisitStepper(state, self.programInfo, testCase);
                auto* result = stepper->step(node);
                if (self.reachabilityEngine != nullptr) {
                    renginePostprocessing(r.first, result);
                }
                return result;
            }

            VisitResult operator()(const TraceEvent* event) {
                CHECK_NULL(event);
                event = event->subst(state.getSymbolicEnv());

                state.add(event);
                state.popBody();
                return new std::vector<VisitBranch>({VisitBranch(&state)});
            }

            VisitResult operator()(Continuation::Return ret) {
                if (ret.expr) {
                    // Step on the returned expression.
                    const auto* expr = *ret.expr;
                    BUG_CHECK(expr, "Attempted to evaluate null expr.");

                    // Do not bother with the stepper, if the expression is already symbolic.
                    if (SymbolicEnv::isSymbolicValue(expr)) {
                        state.popContinuation(expr);
                        return new std::vector<VisitBranch>({VisitBranch(&state)});
                    }

                    auto* stepper = TestgenTarget::getVisitStepper(state, self.programInfo, testCase);
                    auto* result = stepper->step(expr);
                    if (self.reachabilityEngine != nullptr) {
                        ReachabilityResult rresult = std::make_pair(true, nullptr);
                        renginePostprocessing(rresult, result);
                    }
                    return result;
                }

                // Step on valueless return.
                state.popContinuation();
                return new std::vector<VisitBranch>({VisitBranch(&state)});
            }

            VisitResult operator()(Continuation::Exception e) {
                state.handleException(e);
                return new std::vector<VisitBranch>({VisitBranch(&state)});
            }

            VisitResult operator()(const Continuation::PropertyUpdate& e) {
                state.setProperty(e.propertyName, e.property);
                state.popBody();
                return new std::vector<VisitBranch>({VisitBranch(&state)});
            }

            VisitResult operator()(const Continuation::Guard& guard) {
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
                boost::optional<bool> solverVisitResult = boost::none;

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
                    solverVisitResult = self.solver.checkSat(pathConstraints);
                }
                */

                auto* nextState = new VisitState(state);
                nextState->popBody();
                // If we can not solve the guard (either we time out or the solver can not solve the
                // problem) we increment the count of violatedGuardConditions and stop executing
                // this branch.
                if (solverVisitResult == boost::none || !solverVisitResult.get()) {
                    std::stringstream condStream;
                    guard.cond->dbprint(condStream);
                    ::warning(
                        "Guard %1% was not satisfiable."
                        " Incrementing number of guard violations.",
                        condStream.str().c_str());
                    self.violatedGuardConditions++;
                    return new std::vector<VisitBranch>({{IR::getBoolLiteral(false), state, nextState}});
                }
                // Otherwise, we proceed as usual.
                return new std::vector<VisitBranch>({{cond, state, nextState}});
            }

            explicit CommandVisitor(VisitStepEvaluator& self, const TestCase& testCase,
                    VisitState& state)
                : self(self), testCase(testCase), state(state) {}
        } cmdVisitor(*this, testCase, state);

        return boost::apply_visitor(cmdVisitor, *cmdOpt);
    }

    state.popContinuation();
    VisitResult successors = new std::vector<VisitBranch>({VisitBranch(&state)});

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

}  // namespace P4Testgen

}  // namespace P4Tools
