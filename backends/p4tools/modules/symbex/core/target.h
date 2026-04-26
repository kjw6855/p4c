#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_TARGET_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_TARGET_H_

#include <string>

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/core/small_step/expr_stepper.h"
#include "backends/p4tools/modules/symbex/core/small_visit/cmd_visitor.h"
#include "backends/p4tools/modules/symbex/core/small_visit/expr_visitor.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/test_backend.h"

namespace P4::P4Tools::Symbex {

class SymbexTarget : public CompilerTarget {
 public:
    /// @returns the singleton instance for the current target.
    static const SymbexTarget &get();

    /// Produces a @ProgramInfo for the given P4 program.
    ///
    /// @returns nullptr if the program is not supported by this target.
    static const ProgramInfo *produceProgramInfo(const CompilerResult &compilerResult);

    /// Returns the test back end associated with this Symbex target.
    static TestBackEnd *getTestBackend(const ProgramInfo &programInfo,
                                       const TestBackendConfiguration &testBackendConfiguration,
                                       SymbolicExecutor &symbex);

    /// Provides a CmdStepper implementation for this target.
    static CmdStepper *getCmdStepper(ExecutionState &state, AbstractSolver &solver,
                                     const ProgramInfo &programInfo);

    /// Provides a ExprStepper implementation for this target.
    static ExprStepper *getExprStepper(ExecutionState &state, AbstractSolver &solver,
                                       const ProgramInfo &programInfo);

    static CmdVisitor *getCmdVisitor(ExecutionState &state,
                                       const ProgramInfo &programInfo, TestCase &testCase);

    static ExprVisitor *getExprVisitor(ExecutionState &state,
                                       const ProgramInfo &programInfo, TestCase &testCase);

 protected:
    /// @see @produceProgramInfo.
    [[nodiscard]] const ProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult) const;

    /// @see @produceProgramInfo.
    virtual const ProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult, const IR::Declaration_Instance *mainDecl) const = 0;

    /// @see getTestBackend.
    virtual TestBackEnd *getTestBackendImpl(
        const ProgramInfo &programInfo, const TestBackendConfiguration &testBackendConfiguration,
        SymbolicExecutor &symbex) const = 0;

    /// @see getCmdStepper.
    virtual CmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                          const ProgramInfo &programInfo) const = 0;

    /// @see getExprStepper.
    virtual ExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                            const ProgramInfo &programInfo) const = 0;

    /// @see getCmdStepper.
    virtual CmdVisitor *getCmdVisitorImpl(ExecutionState &state,
                                          const ProgramInfo &programInfo, TestCase &testCase) const = 0;

    /// @see getExprVisitor.
    virtual ExprVisitor *getExprVisitorImpl(ExecutionState &state,
                                            const ProgramInfo &programInfo, TestCase &testCase) const = 0;

    explicit SymbexTarget(const std::string &deviceName, const std::string &archName);

    CompilerResultOrError runCompilerImpl(const CompilerOptions &options,
                                          const IR::P4Program *program) const override;

    [[nodiscard]] ICompileContext *makeContext() const override;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_TARGET_H_ */
