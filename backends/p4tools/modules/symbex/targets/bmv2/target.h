#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TARGET_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TARGET_H_

#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/cmd_visitor.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/expr_visitor.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/program_info.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend.h"

namespace P4::P4Tools::Symbex::Bmv2 {

class Bmv2V1ModelSymbexTarget : public SymbexTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const Bmv2V1ModelProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult,
        const IR::Declaration_Instance *mainDecl) const override;

    Bmv2TestBackend *getTestBackendImpl(const ProgramInfo &programInfo,
                                        const TestBackendConfiguration &testBackendConfiguration,
                                        SymbolicExecutor &symbex) const override;

    Bmv2V1ModelCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                             const ProgramInfo &programInfo) const override;

    Bmv2V1ModelExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                               const ProgramInfo &programInfo) const override;

    Bmv2V1ModelCmdVisitor *getCmdVisitorImpl(ExecutionState &state,
                                             const ProgramInfo &programInfo, TestCase &testCase) const override;

    Bmv2V1ModelExprVisitor *getExprVisitorImpl(ExecutionState &state,
                                               const ProgramInfo &programInfo, TestCase &testCase) const override;

 private:
    Bmv2V1ModelSymbexTarget();

    [[nodiscard]] MidEnd mkMidEnd(const CompilerOptions &options) const override;

    CompilerResultOrError runCompilerImpl(const CompilerOptions &options,
                                          const IR::P4Program *program) const override;
};

}  // namespace P4::P4Tools::Symbex::Bmv2

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TARGET_H_ */
