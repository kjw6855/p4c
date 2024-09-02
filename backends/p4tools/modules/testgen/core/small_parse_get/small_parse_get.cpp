#include "backends/p4tools/modules/testgen/core/small_parse_get/small_parse_get.h"

#include <functional>
#include <iosfwd>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "backends/p4tools/common/compiler/reachability.h"
#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "frontends/p4/optimizeExpressions.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/node.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/small_parse_get/cmd_parse_getter.h"
#include "backends/p4tools/modules/testgen/core/small_parse_get/expr_parse_getter.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/continuation.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/options.h"

namespace P4Tools::P4Testgen {

SmallParseGetEvaluator::SmallParseGetEvaluator(const ProgramInfo &programInfo)
    : programInfo(programInfo) {
    if (!TestgenOptions::get().pattern.empty()) {
        reachabilityEngine =
            new ReachabilityEngine(*programInfo.dcg, TestgenOptions::get().pattern, true);
    }
}

void SmallParseGetEvaluator::renginePostprocessing(ReachabilityResult &result,
                                               std::vector<SmallStepEvaluator::Branch> *branches) {
    // All Reachability engine state for branch should be copied.
    if (branches->size() > 1 || result.second != nullptr) {
        for (auto &n : *branches) {
            if (result.second != nullptr) {
                n.constraint = new IR::BAnd(IR::Type_Boolean::get(), n.constraint, result.second);
            }
            if (branches->size() > 1) {
                // Copy reachability engine state
                n.nextState.get().setReachabilityEngineState(
                    n.nextState.get().getReachabilityEngineState()->copy());
            }
        }
    }
}

SmallParseGetEvaluator::REngineType SmallParseGetEvaluator::renginePreprocessing(
    SmallParseGetEvaluator &parse_getter, const ExecutionState &nextState, const IR::Node *node) {
    ReachabilityResult rresult = std::make_pair(true, nullptr);
    std::vector<SmallStepEvaluator::Branch> *branches = nullptr;
    // Current node should be inside DCG.
    if (parse_getter.reachabilityEngine->getDCG().isCaller(node)) {
        // Move reachability engine to next state.
        rresult = parse_getter.reachabilityEngine->next(nextState.getReachabilityEngineState(), node);
        if (!rresult.first) {
            // Reachability property was failed.
            branches = new std::vector<SmallStepEvaluator::Branch>({});
        }
    } else if (const auto *method = node->to<IR::MethodCallStatement>()) {
        return renginePreprocessing(parse_getter, nextState, method->methodCall);
    }
    return std::make_pair(rresult, branches);
}

class CommandParseGetter {
 private:
    std::reference_wrapper<SmallParseGetEvaluator> self;
    ExecutionStateReference state;
    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

 public:
    Result operator()(const IR::Node *node) {
        // ParseGet on the given node as a command.
        BUG_CHECK(node, "Attempted to evaluate null node.");
        SmallParseGetEvaluator::REngineType r;
        if (self.get().reachabilityEngine != nullptr) {
            r = self.get().renginePreprocessing(self, state, node);
            if (r.second != nullptr) {
                return r.second;
            }
        }
        auto *parse_getter =
            TestgenTarget::getCmdParseGetter(state, self.get().programInfo);
        auto *result = parse_getter->parse_get(node);
        if (self.get().reachabilityEngine != nullptr) {
            SmallParseGetEvaluator::renginePostprocessing(r.first, result);
        }
        return result;
    }

    Result operator()(const TraceEvent *event) {
        CHECK_NULL(event);
        event = event->subst(state.get().getSymbolicEnv());

        state.get().add(*event);
        state.get().popBody();
        return new std::vector<Branch>({Branch(state)});
    }

    Result operator()(Continuation::Return ret) {
        if (ret.expr) {
            // ParseGet on the returned expression.
            const auto *expr = *ret.expr;
            BUG_CHECK(expr, "Attempted to evaluate null expr.");
            // Do not bother with the parse_getter, if the expression is already symbolic.
            if (SymbolicEnv::isSymbolicValue(expr)) {
                state.get().popContinuation(expr);
                return new std::vector<Branch>({Branch(state)});
            }
            auto *parse_getter =
                TestgenTarget::getExprParseGetter(state, self.get().programInfo);
            auto *result = parse_getter->parse_get(expr);
            if (self.get().reachabilityEngine != nullptr) {
                ReachabilityResult rresult = std::make_pair(true, nullptr);
                SmallParseGetEvaluator::renginePostprocessing(rresult, result);
            }
            return result;
        }

        // ParseGet on valueless return.
        state.get().popContinuation();
        return new std::vector<Branch>({Branch(state)});
    }

    Result operator()(Continuation::Exception e) {
        state.get().handleException(e);
        return new std::vector<Branch>({Branch(state)});
    }

    Result operator()(const Continuation::PropertyUpdate &e) {
        state.get().setProperty(e.propertyName, e.property);
        state.get().popBody();
        return new std::vector<Branch>({Branch(state)});
    }

    Result operator()(const Continuation::Guard &guard) {
        // Check whether we exceed the number of maximum permitted guard violations.
        // This usually indicates that we have many branches that produce an invalid
        // state.get(). The P4 program should be fixed in that case, because we can not
        // generate useful tests.
        if (self.get().violatedGuardConditions > SmallParseGetEvaluator::MAX_GUARD_VIOLATIONS) {
            BUG("Condition %1% exceeded the maximum number of permitted guard "
                "violations for this run."
                " This implies that the P4 program produces an output that violates"
                " test variants. For example, it may set an output port that is not "
                "testable.",
                guard.cond);
        }

        // Evaluate the guard condition by directly using the solver.
        const auto *cond = guard.cond;
        std::optional<bool> solverResult = std::nullopt;

        // If the guard condition is tainted, treat it equivalent to an invalid state.get().
        cond = state.get().getSymbolicEnv().subst(cond);
        if (!Taint::hasTaint(cond)) {
            cond = P4::optimizeExpression(cond);
            // Check whether the condition is satisfiable in the current execution
            // state.get().
            auto pathConstraints = state.get().getPathConstraint();
            pathConstraints.push_back(cond);
            //solverResult = self.get().solver.checkSat(pathConstraints);
        }

        auto &nextState = state.get().clone();
        nextState.popBody();
        // If we can not solve the guard (either we time out or the solver can not solve
        // the problem) we increment the count of violatedGuardConditions and stop
        // executing this branch.
        if (solverResult == std::nullopt || !solverResult.value()) {
            std::stringstream condStream;
            guard.cond->dbprint(condStream);
            ::warning(
                "Guard %1% was not satisfiable."
                " Incrementing number of guard violations.",
                condStream.str().c_str());
            self.get().violatedGuardConditions++;
            return new std::vector<Branch>({{IR::getBoolLiteral(false), state, nextState}});
        }
        // Otherwise, we proceed as usual.
        return new std::vector<Branch>({{cond, state, nextState}});
    }

    explicit CommandParseGetter(SmallParseGetEvaluator &self, ExecutionState &state)
        : self(self), state(state) {}
};

SmallStepEvaluator::Result SmallParseGetEvaluator::parse_get(ExecutionState &state) {
    BUG_CHECK(!state.isTerminal(), "Tried to parse_get from a terminal state.");

    if (const auto cmdOpt = state.getNextCmd()) {
        return std::visit(CommandParseGetter(*this, state), *cmdOpt);
    }
    // State has an empty body. Pop the continuation stack.
    state.popContinuation();
    return new std::vector<Branch>({Branch(state)});
}

}  // namespace P4Tools::P4Testgen
