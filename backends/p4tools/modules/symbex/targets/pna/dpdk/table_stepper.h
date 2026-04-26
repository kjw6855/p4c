#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_TABLE_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_TABLE_STEPPER_H_

#include "ir/ir.h"

#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/pna/shared_table_stepper.h"

namespace P4::P4Tools::Symbex::Pna {

class PnaDpdkTableStepper : public SharedPnaTableStepper {
 public:
    explicit PnaDpdkTableStepper(PnaDpdkExprStepper *stepper, const IR::P4Table *table);
};

}  // namespace P4::P4Tools::Symbex::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_TABLE_STEPPER_H_ */
