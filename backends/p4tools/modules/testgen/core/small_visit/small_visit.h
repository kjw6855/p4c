#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_VISIT_SMALL_VISIT_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_VISIT_SMALL_VISIT_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "backends/p4tools/common/compiler/reachability.h"
#include "ir/node.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/core/small_step/small_step.h"
#include "backends/p4tools/modules/testgen/p4testgen.pb.h"

using p4testgen::TestCase;

namespace P4Tools::P4Testgen {

/// The main class that implements small-step operational semantics. Delegates to implementations
/// of AbstractVisitor.
class SmallVisitEvaluator {
    friend class CommandVisitor;

 public:
    using Branch = SmallStepEvaluator::Branch;
    using Result = SmallStepEvaluator::Result;

    /// Specifies how many times a guard can be violated in the interpreter until it throws an
    /// error.
    static constexpr uint64_t MAX_GUARD_VIOLATIONS = 100;

 private:
    /// Target-specific information about the P4 program being evaluated.
    const ProgramInfo &programInfo;

    /// The number of times a guard was not satisfiable.
    uint64_t violatedGuardConditions = 0;

    /// Reachability engine.
    ReachabilityEngine *reachabilityEngine = nullptr;

    using RVisitEngineType = std::pair<ReachabilityResult, std::vector<Branch> *>;

    static void renginePostprocessing(ReachabilityResult &result,
                                      std::vector<Branch> *branches);

    RVisitEngineType renginePreprocessing(SmallVisitEvaluator &visitor, const ExecutionState &nextState,
                                     const IR::Node *node);

 public:
    Result step(ExecutionState &state, const TestCase& testCase);

    SmallVisitEvaluator(const ProgramInfo &programInfo);
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_CORE_SMALL_VISIT_SMALL_VISIT_H_ */
