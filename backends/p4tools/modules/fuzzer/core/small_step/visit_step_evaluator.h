#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEP_EVALUTAOR_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEP_EVALUTAOR_H_

#include <cstdint>
#include <vector>

#include <boost/optional/optional.hpp>

#include "backends/p4tools/common/compiler/reachability.h"
#include "backends/p4tools/common/lib/formulae.h"
#include "gsl/gsl-lite.hpp"

#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/small_step.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/p4testgen.pb.h"

using p4testgen::TestCase;

namespace P4Tools {

namespace P4Testgen {

/// The main class that implements small-step operational semantics. Delegates to implementations
/// of AbstractStepper.
class VisitStepEvaluator {
 public:
    /// A branch is an execution state paired with an optional path constraint representing the
    /// choice made to take the branch.
    struct VisitBranch {
        const Constraint* constraint;

        gsl::not_null<VisitState*> nextState;

        /// Simple branch without any constraint.
        explicit VisitBranch(gsl::not_null<VisitState*> nextState);

        /// VisitBranch constrained by a condition. prevState is the state in which the condition
        /// is later evaluated.
        VisitBranch(boost::optional<const Constraint*> c, const VisitState& prevState,
               gsl::not_null<VisitState*> nextState);
    };

    using VisitResult = std::vector<VisitBranch>*;

    /// Specifies how many times a guard can be violated in the interpreter until it throws an
    /// error.
    static constexpr uint64_t MAX_GUARD_VIOLATIONS = 100;

 private:
    /// Target-specific information about the P4 program being evaluated.
    const ProgramInfo& programInfo;

    /// The number of times a guard was not satisfiable.
    uint64_t violatedGuardConditions = 0;

    /// Reachability engine.
    ReachabilityEngine* reachabilityEngine = nullptr;

 public:
    /// Take one step in the program and return list of possible branches.
    VisitResult step(VisitState& state, const TestCase& testCase);

    VisitStepEvaluator(const ProgramInfo& programInfo);
};

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEP_EVALUTAOR_H_ */
