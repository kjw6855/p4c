#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_CMD_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_CMD_STEPPER_H_

#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"

namespace P4::P4Tools::Symbex::Pna {

class SharedPnaCmdStepper : public CmdStepper {
 public:
    SharedPnaCmdStepper(ExecutionState &state, AbstractSolver &solver,
                        const ProgramInfo &programInfo);
};

}  // namespace P4::P4Tools::Symbex::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_SHARED_CMD_STEPPER_H_ */
