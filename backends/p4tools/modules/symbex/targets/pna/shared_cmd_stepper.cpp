#include "backends/p4tools/modules/symbex/targets/pna/shared_cmd_stepper.h"

#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"

namespace P4::P4Tools::Symbex::Pna {

SharedPnaCmdStepper::SharedPnaCmdStepper(ExecutionState &state, AbstractSolver &solver,
                                         const ProgramInfo &programInfo)
    : CmdStepper(state, solver, programInfo) {}

}  // namespace P4::P4Tools::Symbex::Pna
