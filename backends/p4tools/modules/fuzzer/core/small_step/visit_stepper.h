#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEPPER_H_

#include "ir/ir.h"
#include "ir/node.h"
#include "ir/vector.h"
#include "ir/visitor.h"

#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/small_step.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/expr_stepper.h"
#include "backends/p4tools/modules/fuzzer/core/small_step/visit_step_evaluator.h"
#include "backends/p4tools/modules/fuzzer/lib/continuation.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/p4fuzzer.grpc.pb.h"

namespace P4Tools {

namespace P4Testgen {

class VisitStepper : public Inspector {
 private:
    friend class TableVisitor;

 protected:
    struct PacketCursorAdvanceInfo {
        /// How much the parser cursor will be advanced in a successful parsing case.
        int advanceSize;

        /// The condition that needs to be satisfied to successfully advance the parser cursor.
        const IR::Expression* advanceCond;

        /// Specifies at what point the parser cursor advancement will fail.
        int advanceFailSize;

        /// The condition that needs to be satisfied for the advance/extract to be rejected.
        const IR::Expression* advanceFailCond;
    };

 public:
    using VisitBranch = VisitStepEvaluator::VisitBranch;
    using VisitResult = VisitStepEvaluator::VisitResult;
    using TestCase = p4testgen::TestCase;

    VisitResult step(const IR::Node* node);

    bool preorder(const IR::Node* node) override;

    /* markVisited */
    bool preorder(const IR::AssignmentStatement* assign) override;
    bool preorder(const IR::MethodCallStatement* methodCallStatement) override;
    bool preorder(const IR::ExitStatement* e) override;

    bool preorder(const IR::P4Parser* p4parser) override;
    bool preorder(const IR::P4Control* p4control) override;
    bool preorder(const IR::MethodCallExpression* call) override;
    bool preorder(const IR::EmptyStatement* empty) override;
    bool preorder(const IR::IfStatement* ifStatement) override;
    bool preorder(const IR::P4Program* program) override;
    bool preorder(const IR::ParserState* parserState) override;
    bool preorder(const IR::BlockStatement* block) override;
    bool preorder(const IR::SwitchStatement* switchStatement) override;
    bool preorder(const IR::BoolLiteral* boolLiteral) override;
    bool preorder(const IR::Constant* constant) override;
    bool preorder(const IR::Member* member) override;
    bool preorder(const IR::Mux* mux) override;
    bool preorder(const IR::PathExpression* pathExpression) override;

    /// This is a special function that handles the case where structure include P4ValueSet.
    /// Returns an updated structure, replacing P4ValueSet with a list of P4ValueSet components,
    /// splitting the list into separate keys if possible
    bool preorder(const IR::P4ValueSet* valueSet) override;
    bool preorder(const IR::Operation_Binary* binary) override;
    bool preorder(const IR::Operation_Unary* unary) override;
    bool preorder(const IR::SelectExpression* selectExpression) override;
    bool preorder(const IR::ListExpression* listExpression) override;
    bool preorder(const IR::StructExpression* structExpression) override;
    bool preorder(const IR::Slice* slice) override;
    bool preorder(const IR::P4Table* table) override;


    VisitStepper(VisitState& state, const ProgramInfo& programInfo, const TestCase& testCase);

 protected:

    /// Target-specific information about the P4 program being evaluated.
    const ProgramInfo& programInfo;

    /// The state being evaluated.
    VisitState& state;

    /// The output of the evaluation.
    VisitResult result;

    const TestCase& testCase;

    boost::optional<const Constraint*> startParser_impl(const IR::P4Parser* parser,
                                                        VisitState* state) const;

    std::map<Continuation::Exception, Continuation> getExceptionHandlers(
        const IR::P4Parser* parser, Continuation::Body normalContinuation,
        const VisitState* state) const;

    void initializeVariablesFromTestCase(VisitState* nextState, const TestCase& testCase);

    void initializeBlockParams(const IR::Type_Declaration* typeDecl,
                               const std::vector<cstring>* blockParams,
                               VisitState* nextState) const;

    bool stepToException(Continuation::Exception);

    static bool stepToSubexpr(
        const IR::Expression* subexpr, VisitStepEvaluator::VisitResult& result,
        const VisitState& state,
        std::function<const Continuation::Command(const Continuation::Parameter*)> rebuildCmd);

    static bool stepToStructSubexpr(
        const IR::StructExpression* subexpr, VisitStepEvaluator::VisitResult& result,
        const VisitState& state,
        std::function<const Continuation::Command(const IR::StructExpression*)> rebuildCmd);

    static bool stepToListSubexpr(
            const IR::ListExpression* subexpr, VisitStepEvaluator::VisitResult& result,
            const VisitState& state,
            std::function<const Continuation::Command(const IR::ListExpression*)> rebuildCmd);

    bool stepGetHeaderValidity(const IR::Expression* headerRef);
    void setHeaderValidity(const IR::Expression* expr, bool validity, VisitState* state);
    bool stepSetHeaderValidity(const IR::Expression* headerRef, bool validity);
    bool stepStackPushPopFront(const IR::Expression* stackRef, const IR::Vector<IR::Argument>* args,
                               bool isPush = true);

    // Rewrite the list expression to replace the first non-value expression with the continuation
    void logStep(const IR::Node* node);

    const IR::MethodCallStatement* generateStacksetValid(const IR::Expression* stackRef, int index,
                                                     bool isValid);

    void generateStackAssigmentStatement(VisitState* state,
                                     std::vector<Continuation::Command>& replacements,
                                     const IR::Expression* stackRef, int leftIndex,
                                     int rightIndex);

    IR::SwitchStatement* replaceSwitchLabels(const IR::SwitchStatement* switchStatement);

    const Constraint* startParser(const IR::P4Parser* parser, VisitState* nextState);

    void declareVariable(VisitState* nextState, const IR::Declaration_Variable* decl);

    void declareStructLike(VisitState* nextState, const IR::Expression* parentExpr,
                           const IR::Type_StructLike* structType) const;
    void declareBaseType(VisitState* nextState, const IR::Expression* paramPath,
                         const IR::Type_Base* baseType) const;
    static void checkMemberInvariant(const IR::Node* node);

    void handleHitMissActionRun(const IR::Member* member);

    /// Evaluates a call to an extern method that only exists in the interpreter. These are helper
    /// Evaluates a call to an action. This usually only happens when a table is invoked.
    /// In other cases, actions should be inlined. When the action call is evaluated, we use
    /// zombie variables to pass arguments across execution boundaries. These variables persist
    /// until the end of program execution.
    /// @param action the action declaration that is being referenced.
    /// @param call the actual method call containing the arguments.
    void evalActionCall(const IR::P4Action* action, const IR::MethodCallExpression* call);

    virtual void evalExternMethodCall(const IR::MethodCallExpression* call,
                                      const IR::Expression* receiver, IR::ID name,
                                      const IR::Vector<IR::Argument>* args, VisitState& state);

    virtual void evalInternalExternMethodCall(const IR::MethodCallExpression* call,
                                              const IR::Expression* receiver, IR::ID name,
                                              const IR::Vector<IR::Argument>* args,
                                              const VisitState& state);

    virtual PacketCursorAdvanceInfo calculateSuccessfulParserAdvance(const VisitState& state,
                                                                     int advanceSize) const;

    virtual PacketCursorAdvanceInfo calculateAdvanceExpression(
        const VisitState& state, const IR::Expression* advanceExpr,
        const IR::Expression* restrictions) const;

    void generateCopyIn(VisitState* nextState, const IR::Expression* targetPath,
                        const IR::Expression* srcPath, cstring dir, bool forceTaint) const;

    static void setFields(VisitState* nextState,
                          const std::vector<const IR::Member*>& flatFields, int varBitFieldSize);

    const Value* evaluateExpression(const IR::Expression* expr,
                                    boost::optional<const IR::Expression*> cond) const;

    /// Takes a step to reflect a "select" expression failing to match. The default implementation
    /// raises Continuation::Exception::NoMatch.
    virtual void stepNoMatch();

};

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_CORE_SMALL_STEP_VISIT_STEPPER_H_ */
