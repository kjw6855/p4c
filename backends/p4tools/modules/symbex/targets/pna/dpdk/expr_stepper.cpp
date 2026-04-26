#include "backends/p4tools/modules/symbex/targets/pna/dpdk/expr_stepper.h"

#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/table_stepper.h"
#include "backends/p4tools/modules/symbex/targets/pna/shared_expr_stepper.h"

namespace P4::P4Tools::Symbex::Pna {

std::string PnaDpdkExprStepper::getClassName() { return "PnaDpdkExprStepper"; }

PnaDpdkExprStepper::PnaDpdkExprStepper(ExecutionState &state, AbstractSolver &solver,
                                       const ProgramInfo &programInfo)
    : SharedPnaExprStepper(state, solver, programInfo) {}

// Provides implementations of PNA-DPDK externs.
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init)
const ExprStepper::ExternMethodImpls<PnaDpdkExprStepper>
    PnaDpdkExprStepper::PNA_DPDK_EXTERN_METHOD_IMPLS({});

void PnaDpdkExprStepper::evalExternMethodCall(const ExternInfo &externInfo) {
// Remove this once an extern is implemented.
#if 0
    auto method = PNA_DPDK_EXTERN_METHOD_IMPLS.find(
        externInfo.externObjectRef, externInfo.methodName, externInfo.externArguments);
    if (method.has_value()) {
        return method.value()(externInfo, *this);
    }
#endif
    // Lastly, check whether we are calling an internal extern method.
    return SharedPnaExprStepper::evalExternMethodCall(externInfo);
}

bool PnaDpdkExprStepper::preorder(const IR::P4Table *table) {
    return PnaDpdkTableStepper(this, table).eval();
}

}  // namespace P4::P4Tools::Symbex::Pna
