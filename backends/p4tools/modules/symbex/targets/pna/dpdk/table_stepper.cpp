#include "backends/p4tools/modules/symbex/targets/pna/dpdk/table_stepper.h"

#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/targets/pna/shared_table_stepper.h"

namespace P4::P4Tools::Symbex::Pna {

PnaDpdkTableStepper::PnaDpdkTableStepper(PnaDpdkExprStepper *stepper, const IR::P4Table *table)
    : SharedPnaTableStepper(stepper, table) {}

}  // namespace P4::P4Tools::Symbex::Pna
