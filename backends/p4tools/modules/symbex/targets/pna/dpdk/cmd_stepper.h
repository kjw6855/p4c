#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_CMD_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_CMD_STEPPER_H_

#include <map>
#include <optional>
#include <string>

#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/program_info.h"
#include "backends/p4tools/modules/symbex/targets/pna/shared_cmd_stepper.h"

namespace P4::P4Tools::Symbex::Pna {

class PnaDpdkCmdStepper : public SharedPnaCmdStepper {
 protected:
    std::string getClassName() override;

    const PnaDpdkProgramInfo &getProgramInfo() const override;

    void initializeTargetEnvironment(ExecutionState &nextState) const override;

    std::optional<const Constraint *> startParserImpl(const IR::P4Parser *parser,
                                                      ExecutionState &state) const override;

    std::map<Continuation::Exception, Continuation> getExceptionHandlers(
        const IR::P4Parser *parser, Continuation::Body normalContinuation,
        const ExecutionState &state) const override;

 public:
    PnaDpdkCmdStepper(ExecutionState &state, AbstractSolver &solver,
                      const ProgramInfo &programInfo);
};

}  // namespace P4::P4Tools::Symbex::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_PNA_DPDK_CMD_STEPPER_H_ */
