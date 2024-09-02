#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_PARSE_GET_SMALL_STEP_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_PARSE_GET_SMALL_STEP_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "backends/p4tools/common/compiler/reachability.h"
#include "backends/p4tools/common/core/solver.h"
#include "ir/node.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/core/small_step/small_step.h"
#include "lib/safe_vector.h"

namespace P4Tools::P4Testgen {

/// The main class that implements small-parse_get operational semantics. Delegates to implementations
/// of AbstractParseGetter.
class SmallParseGetEvaluator {
    friend class CommandParseGetter;

 public:
    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

    /// Specifies how many times a guard can be violated in the interpreter until it throws an
    /// error.
    static constexpr uint64_t MAX_GUARD_VIOLATIONS = 100;

 private:
    /// Target-specific information about the P4 program being evaluated.
    const ProgramInfo &programInfo;

    /// The solver backing this evaluator.
    //AbstractSolver &solver;

    /// The number of times a guard was not satisfiable.
    uint64_t violatedGuardConditions = 0;

    /// Reachability engine.
    ReachabilityEngine *reachabilityEngine = nullptr;

    using REngineType = std::pair<ReachabilityResult, std::vector<SmallStepEvaluator::Branch> *>;

    static void renginePostprocessing(ReachabilityResult &result,
                                      std::vector<SmallStepEvaluator::Branch> *branches);

    REngineType renginePreprocessing(SmallParseGetEvaluator &parse_getter, const ExecutionState &nextState,
                                     const IR::Node *node);

 public:
    Result parse_get(ExecutionState &state);

    SmallParseGetEvaluator(const ProgramInfo &programInfo);
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_PARSE_GET_SMALL_STEP_H_ */
