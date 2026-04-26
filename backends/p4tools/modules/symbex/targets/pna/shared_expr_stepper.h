#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_EXPR_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_EXPR_STEPPER_H_

#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/expr_stepper.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"

namespace P4::P4Tools::Symbex::Pna {

class SharedPnaExprStepper : public ExprStepper {
 private:
    // Provides implementations of common PNA externs.
    static const ExternMethodImpls<SharedPnaExprStepper> PNA_EXTERN_METHOD_IMPLS;

 public:
    SharedPnaExprStepper(ExecutionState &state, AbstractSolver &solver,
                         const ProgramInfo &programInfo);

    void evalExternMethodCall(const ExternInfo &externInfo) override;

    bool preorder(const IR::P4Table * /*table*/) override;
};
}  // namespace P4::P4Tools::Symbex::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_EXPR_STEPPER_H_ */
