#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_EXPR_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_EXPR_STEPPER_H_

#include <string>

#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/pna/shared_expr_stepper.h"

namespace P4::P4Tools::Symbex::Pna {

class PnaDpdkExprStepper : public SharedPnaExprStepper {
 protected:
    std::string getClassName() override;

 private:
    // Provides implementations of PNA-DPDK externs.
    static const ExternMethodImpls<PnaDpdkExprStepper> PNA_DPDK_EXTERN_METHOD_IMPLS;

 public:
    PnaDpdkExprStepper(ExecutionState &state, AbstractSolver &solver,
                       const ProgramInfo &programInfo);

    void evalExternMethodCall(const ExternInfo &externInfo) override;

    bool preorder(const IR::P4Table * /*table*/) override;
};
}  // namespace P4::P4Tools::Symbex::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_EXPR_STEPPER_H_ */
