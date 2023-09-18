#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_CMD_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_CMD_STEPPER_H_

#include <map>
#include <optional>
#include <string>

#include "backends/p4tools/common/core/solver.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/testgen/lib/continuation.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/targets/tna/program_info.h"

namespace P4Tools::P4Testgen::Tna {

class TnaCmdStepper : public CmdStepper {
 protected:
    std::string getClassName() override { return "TnaCmdStepper"; }

    const TnaProgramInfo &getProgramInfo() const override;

    void initializeTargetEnvironment(ExecutionState &nextState) const override;

    std::optional<const Constraint *> startParserImpl(const IR::P4Parser *parser,
                                                      ExecutionState &nextState) const override;

    std::map<Continuation::Exception, Continuation> getExceptionHandlers(
        const IR::P4Parser *parser, Continuation::Body normalContinuation,
        const ExecutionState &nextState) const override;

 public:
    TnaCmdStepper(ExecutionState &nextState, AbstractSolver &solver,
                          const ProgramInfo &programInfo);
};

}  // namespace P4Tools::P4Testgen::Tna

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_CMD_STEPPER_H_ */
