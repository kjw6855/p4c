#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SMALL_STEP_CMD_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SMALL_STEP_CMD_STEPPER_H_

#include <map>
#include <optional>
#include <vector>

#include "ir/ir.h"
#include "ir/solver.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/abstract_stepper.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"

namespace P4::P4Tools::Symbex {

/// Implements small-step operational semantics for commands.
class CmdStepper : public AbstractStepper {
 public:
    CmdStepper(ExecutionState &state, AbstractSolver &solver, const ProgramInfo &programInfo);

    bool preorder(const IR::AssignmentStatement *assign) override;
    bool preorder(const IR::P4Parser *p4parser) override;
    bool preorder(const IR::P4Control *p4control) override;
    bool preorder(const IR::EmptyStatement *empty) override;
    bool preorder(const IR::IfStatement *ifStatement) override;
    bool preorder(const IR::MethodCallStatement *methodCallStatement) override;
    bool preorder(const IR::P4Program *program) override;
    bool preorder(const IR::ParserState *parserState) override;
    bool preorder(const IR::BlockStatement *block) override;
    bool preorder(const IR::ExitStatement *e) override;
    bool preorder(const IR::SwitchStatement *switchStatement) override;

    /// Boolean state variable recording which branch of @p ifStatement was taken (true = then,
    /// false = else). Keyed by the if-statement's source position so it is stable across the
    /// condition-reduction clone the stepper makes. Stamped only under the STATE_DEP_TAMPERING_COND
    /// policy; read by the H2S2C tampering generator (analogous to TableStepper::getTableHitVar).
    static const IR::StateVariable &getConditionVar(const IR::IfStatement *ifStatement);

 protected:
    /// Initializes the given state for entry into the given parser.
    ///
    /// @returns constraints for associating packet data with symbolic state.
    const Constraint *startParser(const IR::P4Parser *parser, ExecutionState &state);

    /// @see startParser. Implementations can assume that the parser has been registered, and the
    /// cursor position has been initialized.
    virtual std::optional<const Constraint *> startParserImpl(const IR::P4Parser *parser,
                                                              ExecutionState &state) const = 0;

    /// Initializes variables and adds constraints for the program initialization, which is target
    /// specific.
    virtual void initializeTargetEnvironment(ExecutionState &state) const = 0;

    /// Provides exception-handler implementations for the given parser.
    ///
    /// @param normalContinuation is the continuation that would be executed if the parser finishes
    ///     normally.
    virtual std::map<Continuation::Exception, Continuation> getExceptionHandlers(
        const IR::P4Parser *parser, Continuation::Body normalContinuation,
        const ExecutionState &state) const = 0;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SMALL_STEP_CMD_STEPPER_H_ */
