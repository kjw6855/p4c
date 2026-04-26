#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_CMD_VISITOR_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_CMD_VISITOR_H_

#include <map>
#include <optional>
#include <string>

#include "ir/ir.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_visit/cmd_visitor.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/program_info.h"

namespace P4::P4Tools::Symbex::Bmv2 {

class Bmv2V1ModelCmdVisitor : public CmdVisitor {
 protected:
    std::string getClassName() override { return "Bmv2V1ModelCmdVisitor"; }

    const Bmv2V1ModelProgramInfo &getProgramInfo() const override;

    void initializeTargetEnvironment(ExecutionState &nextState, TestCase &testCase) const override;

    std::optional<const Constraint *> startParserImpl(const IR::P4Parser *parser,
                                                      ExecutionState &nextState) const override;

    std::map<Continuation::Exception, Continuation> getExceptionHandlers(
        const IR::P4Parser *parser, Continuation::Body normalContinuation,
        const ExecutionState &nextState) const override;

 public:
    Bmv2V1ModelCmdVisitor(ExecutionState &nextState,
                          const ProgramInfo &programInfo, TestCase &testCase);
};

}  // namespace P4::P4Tools::Symbex::Bmv2

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_CMD_VISITOR_H_ */
