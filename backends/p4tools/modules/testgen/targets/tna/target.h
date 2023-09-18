#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TARGET_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TARGET_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/lib/arch_spec.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/targets/tna/cmd_stepper.h"
#include "backends/p4tools/modules/testgen/targets/tna/expr_stepper.h"
#include "backends/p4tools/modules/testgen/targets/tna/cmd_visitor.h"
#include "backends/p4tools/modules/testgen/targets/tna/expr_visitor.h"
#include "backends/p4tools/modules/testgen/targets/tna/program_info.h"
#include "backends/p4tools/modules/testgen/targets/tna/test_backend.h"

namespace P4Tools::P4Testgen::Tna {

class TnaTestgenTarget : public TestgenTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const TnaProgramInfo *initProgramImpl(
        const IR::P4Program *program, const IR::Declaration_Instance *mainDecl) const override;

    TnaTestBackend *getTestBackendImpl(const ProgramInfo &programInfo, SymbolicExecutor &symbex,
                                        const std::filesystem::path &testPath) const override;

    TnaCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                             const ProgramInfo &programInfo) const override;

    TnaExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                               const ProgramInfo &programInfo) const override;

    TnaCmdVisitor *getCmdVisitorImpl(ExecutionState &state,
                                             const ProgramInfo &programInfo, TestCase &testCase) const override;

    TnaExprVisitor *getExprVisitorImpl(ExecutionState &state,
                                               const ProgramInfo &programInfo, TestCase &testCase) const override;

    [[nodiscard]] const ArchSpec *getArchSpecImpl() const override;

 private:
    TnaTestgenTarget();

    static const ArchSpec ARCH_SPEC;
};

}  // namespace P4Tools::P4Testgen::Tna

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TARGET_H_ */
