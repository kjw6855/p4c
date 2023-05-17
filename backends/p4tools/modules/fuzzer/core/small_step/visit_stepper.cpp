#include "backends/p4tools/modules/fuzzer/core/small_step/visit_stepper.h"

#include "backends/p4tools/common/compiler/convert_hs_index.h"
#include "backends/p4tools/common/lib/util.h"

#include "ir/dump.h"
#include "ir/declaration.h"
#include "ir/id.h"
#include "ir/indexed_vector.h"
#include "ir/irutils.h"
#include "ir/node.h"
#include "ir/vector.h"
#include "lib/cstring.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/null.h"
#include "lib/safe_vector.h"
#include "midend/saturationElim.h"

#include "backends/p4tools/modules/fuzzer/core/small_step/table_visitor.h"
#include "backends/p4tools/modules/fuzzer/core/target.h"
#include "backends/p4tools/modules/fuzzer/lib/exceptions.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/lib/gen_eq.h"

#include "backends/p4tools/modules/fuzzer/targets/bmv2/constants.h"
#include "backends/p4tools/modules/fuzzer/targets/bmv2/program_info.h"

#include "backends/p4tools/modules/fuzzer/p4fuzzer.grpc.pb.h"
#include "backends/p4tools/modules/fuzzer/p4testgen.pb.h"

namespace P4Tools {

namespace P4Testgen {

VisitStepper::VisitStepper(VisitState& state, const ProgramInfo& programInfo,
        const TestCase& testCase)
    : programInfo(programInfo), state(state), testCase(testCase), result(new std::vector<VisitBranch>()) {}

VisitStepper::VisitResult VisitStepper::step(const IR::Node* node) {
    std::cout << "[" << node->node_type_name() << "]";
    if (dynamic_cast<const IR::P4Program*>(node) == nullptr &&
            dynamic_cast<const IR::P4Control*>(node) == nullptr &&
            dynamic_cast<const IR::P4Parser*>(node) == nullptr) {
        std::cout << " " << node;
    }
    std::cout << std::endl;

    node->apply(*this);
    return result;
}

/******* Protected inner functions *******/
void VisitStepper::logStep(const IR::Node* node) {
    LOG_FEATURE("small_step", 4,
                "***** " << node->node_type_name() << " *****"
                         << std::endl
                         << node << std::endl);
}

const Value* VisitStepper::evaluateExpression(
    const IR::Expression* expr, boost::optional<const IR::Expression*> cond) const {

    // TODO: solver is needed??
    return nullptr;

#if 0
    BUG_CHECK(solver.isInIncrementalMode(),
              "Currently, expression valuation only supports an incremental solver.");
    auto constraints = state.getPathConstraint();
    expr = state.getSymbolicEnv().subst(expr);
    expr = P4::optimizeExpression(expr);
    // Assert the path constraint to the solver and check whether it is satisfiable.
    if (cond) {
        constraints.push_back(*cond);
    }
    auto solverVisitResult = solver.checkSat(constraints);
    // If the solver can find a solution under the given condition, get the model and return the
    // value.
    const Value* result = nullptr;
    if (solverVisitResult != boost::none && *solverVisitResult) {
        auto model = *solver.getModel();
        model.complete(expr);
        result = model.evaluate(expr);
    }
    return result;
#endif
}

boost::optional<const Constraint*> VisitStepper::startParser_impl(
    const IR::P4Parser* parser, VisitState* nextState) const {
    // We need to explicitly map the parser error
    const auto* errVar =
        programInfo.getParserParamVar(parser, programInfo.getParserErrorType(), 3, "parser_error");
    nextState->setParserErrorLabel(errVar);

    return boost::none;
}


std::map<Continuation::Exception, Continuation> VisitStepper::getExceptionHandlers(
    const IR::P4Parser* parser, Continuation::Body /*normalContinuation*/,
    const VisitState* /*nextState*/) const {
    std::map<Continuation::Exception, Continuation> result;
    const auto targetProgramInfo = *programInfo.checkedTo<Bmv2::BMv2_V1ModelProgramInfo>();
    auto gress = targetProgramInfo.getGress(parser);

    const auto* errVar =
        targetProgramInfo.getParserParamVar(parser, targetProgramInfo.getParserErrorType(), 3, "parser_error");

    switch (gress) {
        case Bmv2::BMV2_INGRESS:
            // TODO: Implement the conditions above. Currently, this always drops the packet.
            // Suggest refactoring `result` so that its values are lists of (path condition,
            // continuation) pairs. This would allow us to branch on the value of parser_err.
            // Would also need to augment ProgramInfo to detect whether the ingress MAU refers
            // to parser_err.

            ::warning("Ingress parser exception handler not fully implemented");
            result.emplace(Continuation::Exception::Reject, Continuation::Body({}));
            result.emplace(Continuation::Exception::PacketTooShort,
                           Continuation::Body({new IR::AssignmentStatement(
                               errVar, IR::getConstant(errVar->type, 1))}));
            // NoMatch will transition to the next block.
            result.emplace(Continuation::Exception::NoMatch, Continuation::Body({}));
            break;

        // The egress parser never drops the packet.
        case Bmv2::BMV2_EGRESS:
            // NoMatch will transition to the next block.
            result.emplace(Continuation::Exception::NoMatch, Continuation::Body({}));
            break;
        default:
            BUG("Unimplemented thread: %1%", gress);
    }

    return result;
}

void VisitStepper::initializeVariablesFromTestCase(VisitState* nextState, const TestCase& testCase) {
    /**
     * XXX: Target-aware initialization (BMv2-specific. Tofino?)
     * Copied from void BMv2_V1ModelCmdStepper::initializeTargetEnvironment(VisitState* nextState);
     */
    const auto* archSpec = TestgenTarget::getArchSpec();
    const auto targetProgramInfo = *programInfo.checkedTo<Bmv2::BMv2_V1ModelProgramInfo>();
    const auto* programmableBlocks = targetProgramInfo.getProgrammableBlocks();
    //const auto* programmableBlocks = BMv2_V1ModelProgramInfo::getProgrammableBlocks();
    size_t blockIdx = 0;
    for (const auto& blockTuple : *programmableBlocks) {
        const auto* typeDecl = blockTuple.second;
        const auto* archMember = archSpec->getArchMember(blockIdx);
        initializeBlockParams(typeDecl, &archMember->blockParams, nextState);
        blockIdx++;
    }

    const auto* nineBitType = IR::getBitType(9);
    const auto* oneBitType = IR::getBitType(1);
    //nextState->set(targetProgramInfo.getTargetInputPortVar(),
    //              nextState->createZombieConst(nineBitType, "*bmv2_ingress_port"));
    //const auto* givenInputPort = new IR::Equ(targetProgramInfo.getTargetInputPortVar(),
    //        IR::getConstant(nineBitType, std::stoi(testCase.input_packet().port())));
    nextState->set(targetProgramInfo.getTargetInputPortVar(),
            IR::getConstant(nineBitType, std::stoi(testCase.input_packet().port())));

    const auto& inputPacket = testCase.input_packet().packet();

    // Set packet size
    const auto* pktSizeType = VisitState::getPacketSizeVarType();
    const auto* packetSizeVar =
        new IR::Member(pktSizeType, new IR::PathExpression("*standard_metadata"), "packet_length");
    nextState->set(packetSizeVar,
            IR::getConstant(pktSizeType, inputPacket.length()));

    nextState->setInputPacketSize(inputPacket.length() * 8);

    // Set packet buffer
    nextState->appendToPacketBuffer(Utils::getValExpr(inputPacket, inputPacket.length() * 8));

    // BMv2 implicitly sets the output port to 0.
    nextState->set(targetProgramInfo.getTargetOutputPortVar(), IR::getConstant(nineBitType, 0));
    // Initialize parser_err with no error.
    const auto* parserErrVar =
        new IR::Member(targetProgramInfo.getParserErrorType(),
                       new IR::PathExpression("*standard_metadata"), "parser_error");
    nextState->set(parserErrVar, IR::getConstant(parserErrVar->type, 0));
    // Initialize checksum_error with no error.
    const auto* checksumErrVar =
        new IR::Member(oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
    nextState->set(checksumErrVar, IR::getConstant(checksumErrVar->type, 0));
}


void VisitStepper::initializeBlockParams(const IR::Type_Declaration* typeDecl,
                                       const std::vector<cstring>* blockParams,
                                       VisitState* nextState) const {
    // Collect parameters.
    const auto* iApply = typeDecl->to<IR::IApply>();
    BUG_CHECK(iApply != nullptr, "Constructed type %s of type %s not supported.", typeDecl,
              typeDecl->node_type_name());
    // Also push the namespace of the respective parameter.
    nextState->pushNamespace(typeDecl->to<IR::INamespace>());
    // Collect parameters.
    const auto* params = iApply->getApplyParameters();
    for (size_t paramIdx = 0; paramIdx < params->size(); ++paramIdx) {
        const auto* param = params->getParameter(paramIdx);
        const auto* paramType = param->type;
        // Retrieve the identifier of the global architecture map using the parameter index.
        auto archRef = blockParams->at(paramIdx);
        // Irrelevant parameter. Ignore.
        if (archRef == nullptr) {
            continue;
        }
        // We need to resolve type names.
        paramType = nextState->resolveType(paramType);
        const auto* paramPath = new IR::PathExpression(paramType, new IR::Path(archRef));
        if (const auto* ts = paramType->to<IR::Type_StructLike>()) {
            declareStructLike(nextState, paramPath, ts);
        } else if (const auto* tb = paramType->to<IR::Type_Base>()) {
            // If the type is a flat Type_Base, postfix it with a "*".
            const auto& paramRef = Utils::addZombiePostfix(paramPath, tb);
            nextState->set(paramRef, programInfo.createTargetUninitialized(paramType, false));
        } else {
            P4C_UNIMPLEMENTED("Unsupported initialization type %1%", paramType->node_type_name());
        }
    }
}

void VisitStepper::checkMemberInvariant(const IR::Node* node) {
    while (true) {
        const auto* member = node->to<IR::Member>();
        BUG_CHECK(member, "Not a member expression: %1%", node);

        node = member->expr;
        if (node->is<IR::PathExpression>()) {
            return;
        }
    }
}

bool VisitStepper::stepToException(Continuation::Exception exception) {
    state.replaceTopBody(exception);
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::stepToSubexpr(
    const IR::Expression* subexpr, VisitStepEvaluator::VisitResult& result, const VisitState& state,
    std::function<const Continuation::Command(const Continuation::Parameter*)> rebuildCmd) {
    // Create a parameter for the continuation we're about to build.
    const auto* v = Continuation::genParameter(subexpr->type, "v", state.getNamespaceContext());

    // Create the continuation itself.
    Continuation::Body kBody(state.getBody());
    kBody.pop();
    kBody.push(rebuildCmd(v));
    Continuation k(v, kBody);

    // Create our new state.
    auto* nextState = new VisitState(state);
    Continuation::Body stateBody({Continuation::Return(subexpr)});
    nextState->replaceBody(stateBody);
    nextState->pushContinuation(new VisitState::StackFrame(k, state.getNamespaceContext()));

    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::stepToListSubexpr(
    const IR::ListExpression* subexpr, VisitStepEvaluator::VisitResult& result,
    const VisitState& state,
    std::function<const Continuation::Command(const IR::ListExpression*)> rebuildCmd) {
    // Rewrite the list expression to replace the first non-value expression with the continuation
    // parameter.
    // XXX This results in the small-step evaluator evaluating list expressions in quadratic time.
    // XXX Ideally, we'd build a tower of continuations, with each non-value component of the list
    // XXX expression being evaluated in a separate continuation, and piece the final list
    // XXX expression back together in the last continuation. But the evaluation framework isn't
    // XXX currently rich enough to express this: we'd need lambdas in our continuation bodies.
    // XXX
    // XXX Alternatively, we could introduce P4 variables to store the values obtained by each
    // XXX intermediate continuation, but we'd have to be careful about how those variables are
    // XXX named.
    // XXX
    // XXX For now, we do the quadratic-time thing, since it's easiest.

    // Find the first non-value component of the list.
    const auto& components = subexpr->components;
    const IR::Expression* nonValueComponent = nullptr;
    size_t nonValueComponentIdx = 0;
    for (nonValueComponentIdx = 0; nonValueComponentIdx < components.size();
         nonValueComponentIdx++) {
        const auto* curComponent = components.at(nonValueComponentIdx);
        if (!SymbolicEnv::isSymbolicValue(curComponent)) {
            nonValueComponent = curComponent;
            break;
        }
    }

    BUG_CHECK(nonValueComponent != nullptr && nonValueComponentIdx != components.size(),
              "This list expression is a symbolic value, but is not expected to be one: %1%",
              subexpr);

    return stepToSubexpr(
        nonValueComponent, result, state,
        [nonValueComponentIdx, rebuildCmd, subexpr](const Continuation::Parameter* v) {
            auto* result = subexpr->clone();
            result->components[nonValueComponentIdx] = v->param;
            return rebuildCmd(result);
        });
}

bool VisitStepper::stepToStructSubexpr(
    const IR::StructExpression* subexpr, VisitStepEvaluator::VisitResult& result,
    const VisitState& state,
    std::function<const Continuation::Command(const IR::StructExpression*)> rebuildCmd) {
    // @see stepToListSubexpr regarding the assessment of complexity of this function.

    // Find the first non-value component of the list.
    const auto& components = subexpr->components;
    const IR::Expression* nonValueComponent = nullptr;
    size_t nonValueComponentIdx = 0;
    for (nonValueComponentIdx = 0; nonValueComponentIdx < components.size();
         nonValueComponentIdx++) {
        const auto* curComponent = components.at(nonValueComponentIdx);
        if (!SymbolicEnv::isSymbolicValue(curComponent->expression)) {
            nonValueComponent = curComponent->expression;
            break;
        }
    }

    BUG_CHECK(nonValueComponent != nullptr && nonValueComponentIdx != components.size(),
              "This list expression is a symbolic value, but is not expected to be one: %1%",
              subexpr);

    return stepToSubexpr(
        nonValueComponent, result, state,
        [nonValueComponentIdx, rebuildCmd, subexpr](const Continuation::Parameter* v) {
            auto* result = subexpr->clone();
            auto* namedClone = result->components[nonValueComponentIdx]->clone();
            namedClone->expression = v->param;
            result->components[nonValueComponentIdx] = namedClone;
            return rebuildCmd(result);
        });
}

bool VisitStepper::stepGetHeaderValidity(const IR::Expression* headerRef) {
    // The top of the body should be a Return command containing a call to getValid on the given
    // header ref. Replace this with the variable representing the header ref's validity.
    if (const auto* headerUnion = headerRef->type->to<IR::Type_HeaderUnion>()) {
        for (const auto* field : headerUnion->fields) {
            auto* fieldRef = new IR::Member(field->type, headerRef, field->name);
            auto variable = Utils::getHeaderValidity(fieldRef);
            BUG_CHECK(state.exists(variable),
                      "At this point, the header validity bit should be initialized.");
            const auto* value = state.getSymbolicEnv().get(variable);
            const auto* res = value->to<IR::BoolLiteral>();
            BUG_CHECK(res, "%1%: expected a boolean", value);
            if (res->value) {
                state.replaceTopBody(
                    Continuation::Return(new IR::BoolLiteral(IR::Type::Boolean::get(), true)));
                result->emplace_back(&state);
                return false;
            }
        }
        state.replaceTopBody(
            Continuation::Return(new IR::BoolLiteral(IR::Type::Boolean::get(), false)));
        result->emplace_back(&state);
        return false;
    }
    auto variable = Utils::getHeaderValidity(headerRef);
    BUG_CHECK(state.exists(variable),
              "At this point, the header validity bit should be initialized.");
    state.replaceTopBody(Continuation::Return(variable));
    result->emplace_back(&state);
    return false;
}

const IR::MethodCallStatement* VisitStepper::generateStacksetValid(const IR::Expression* stackRef, int index,
                                                     bool isValid) {
    const auto* arrayIndex = HSIndexToMember::produceStackIndex(
        stackRef->type->checkedTo<IR::Type_Stack>()->elementType, stackRef, index);
    auto name = (isValid) ? IR::Type_Header::setValid : IR::Type_Header::setInvalid;
    return new IR::MethodCallStatement(new IR::MethodCallExpression(
        new IR::Type_Void(),
        new IR::Member(new IR::Type_Method(new IR::Type_Void(), new IR::ParameterList(), name),
                       arrayIndex, name)));
}

void VisitStepper::generateStackAssigmentStatement(VisitState* state,
                                     std::vector<Continuation::Command>& replacements,
                                     const IR::Expression* stackRef, int leftIndex,
                                     int rightIndex) {
    const auto* elemType = stackRef->type->checkedTo<IR::Type_Stack>()->elementType;
    const auto* leftArIndex = HSIndexToMember::produceStackIndex(elemType, stackRef, leftIndex);
    const auto* rightArrIndex = HSIndexToMember::produceStackIndex(elemType, stackRef, rightIndex);

    // Check right header validity.
    const auto* value = state->getSymbolicEnv().get(Utils::getHeaderValidity(rightArrIndex));
    if (!value->checkedTo<IR::BoolLiteral>()->value) {
        replacements.emplace_back(generateStacksetValid(stackRef, leftIndex, false));
        return;
    }
    replacements.emplace_back(generateStacksetValid(stackRef, leftIndex, true));

    // Unfold fields.
    const auto* structType = elemType->checkedTo<IR::Type_StructLike>();
    auto leftVector = state->getFlatFields(leftArIndex, structType);
    auto rightVector = state->getFlatFields(rightArrIndex, structType);
    for (size_t i = 0; i < leftVector.size(); i++) {
        replacements.emplace_back(new IR::AssignmentStatement(leftVector[i], rightVector[i]));
    }
}

bool VisitStepper::stepStackPushPopFront(const IR::Expression* stackRef,
                                            const IR::Vector<IR::Argument>* args, bool isPush) {
    const auto* stackType = stackRef->type->checkedTo<IR::Type_Stack>();
    auto sz = static_cast<int>(stackType->getSize());
    BUG_CHECK(args->size() == 1, "Invalid size of arguments for %1%", stackRef);
    auto count = args->at(0)->expression->checkedTo<IR::Constant>()->asInt();
    std::vector<Continuation::Command> replacements;
    auto* nextState = new VisitState(state);
    if (isPush) {
        for (int i = sz - 1; i >= 0; i -= 1) {
            if (i >= count) {
                generateStackAssigmentStatement(nextState, replacements, stackRef, i, i - count);
            } else {
                replacements.emplace_back(generateStacksetValid(stackRef, i, false));
            }
        }
    } else {
        for (int i = 0; i < sz; i++) {
            if (i + count < sz) {
                generateStackAssigmentStatement(nextState, replacements, stackRef, i, i + count);
            } else {
                replacements.emplace_back(generateStacksetValid(stackRef, i, false));
            }
        }
    }
    nextState->replaceTopBody(&replacements);
    result->emplace_back(nextState);
    return false;
}

void VisitStepper::setHeaderValidity(const IR::Expression* expr, bool validity,
                                        VisitState* nextState) {
    auto headerRefValidity = Utils::getHeaderValidity(expr);
    nextState->set(headerRefValidity, IR::getBoolLiteral(validity));

    // In some cases, the header may be part of a union.
    if (validity) {
        const auto* headerBaseMember = expr->to<IR::Member>();
        if (headerBaseMember == nullptr) {
            return;
        }
        const auto* headerBase = headerBaseMember->expr;
        // In the case of header unions, we need to set all other union members invalid.
        if (const auto* hdrUnion = headerBase->type->to<IR::Type_HeaderUnion>()) {
            for (const auto* field : hdrUnion->fields) {
                auto* member = new IR::Member(field->type, headerBase, field->name);
                // Ignore the member we are setting to valid.
                if (expr->equiv(*member)) {
                    continue;
                }
                // Set all other members to invalid.
                setHeaderValidity(member, false, nextState);
            }
        }
        return;
    }
    const auto* exprType = expr->type->checkedTo<IR::Type_StructLike>();
    std::vector<const IR::Member*> validityVector;
    auto fieldsVector = nextState->getFlatFields(expr, exprType, &validityVector);
    // The header is going to be invalid. Set all fields to taint constants.
    // TODO: Should we make this target specific? Some targets set the header fields to 0.
    for (const auto* field : fieldsVector) {
        nextState->set(field, programInfo.createTargetUninitialized(field->type, true));
    }
}

bool VisitStepper::stepSetHeaderValidity(const IR::Expression* headerRef, bool validity) {
    // The top of the body should be a Return command containing a call to setValid or setInvalid
    // on the given header ref. Update the symbolic environment to reflect the changed validity
    // bit, and replace the command with an expressionless Return.
    setHeaderValidity(headerRef, validity, &state);
    state.replaceTopBody(Continuation::Return());
    result->emplace_back(&state);
    return false;
}

void VisitStepper::declareStructLike(VisitState* nextState, const IR::Expression* parentExpr,
                                        const IR::Type_StructLike* structType) const {
    std::vector<const IR::Member*> validFields;
    auto fields = nextState->getFlatFields(parentExpr, structType, &validFields);
    // We also need to initialize the validity bits of the headers. These are false.
    for (const auto* validField : validFields) {
        nextState->set(validField, IR::getBoolLiteral(false));
    }
    // For each field in the undefined struct, we create a new zombie variable.
    // If the variable does not have an initializer we need to create a new zombie for it.
    // For now we just use the name directly.
    for (const auto* field : fields) {
        nextState->set(field, programInfo.createTargetUninitialized(field->type, false));
    }
}

void VisitStepper::declareBaseType(VisitState* nextState, const IR::Expression* paramPath,
                                      const IR::Type_Base* baseType) const {
    nextState->set(paramPath, programInfo.createTargetUninitialized(baseType, false));
}

void VisitStepper::declareVariable(VisitState* nextState, const IR::Declaration_Variable* decl) {
    if (decl->initializer != nullptr) {
        TESTGEN_UNIMPLEMENTED("Unsupported initializer %s for declaration variable.",
                              decl->initializer);
    }
    const auto* declType = state.resolveType(decl->type);

    if (const auto* structType = declType->to<IR::Type_StructLike>()) {
        const auto* parentExpr = new IR::PathExpression(structType, new IR::Path(decl->name.name));
        declareStructLike(nextState, parentExpr, structType);
    } else if (const auto* stackType = declType->to<IR::Type_Stack>()) {
        const auto* stackSizeExpr = stackType->size;
        auto stackSize = stackSizeExpr->checkedTo<IR::Constant>()->asInt();
        const auto* stackElemType = stackType->elementType;
        if (stackElemType->is<IR::Type_Name>()) {
            stackElemType = nextState->resolveType(stackElemType->to<IR::Type_Name>());
        }
        const auto* structType = stackElemType->checkedTo<IR::Type_StructLike>();
        for (auto idx = 0; idx < stackSize; idx++) {
            const auto* parentExpr = HSIndexToMember::produceStackIndex(
                structType, new IR::PathExpression(stackType, new IR::Path(decl->name.name)), idx);
            declareStructLike(&state, parentExpr, structType);
        }
    } else if (declType->is<IR::Type_Base>()) {
        // If the variable does not have an initializer we need to create a new zombie for it.
        // For now we just use the name directly.
        const auto& left = nextState->convertPathExpr(new IR::PathExpression(decl->name));
        nextState->set(left, programInfo.createTargetUninitialized(decl->type, false));
    } else {
        TESTGEN_UNIMPLEMENTED("Unsupported declaration type %1% node: %2%", declType,
                              declType->node_type_name());
    }
}

/******* PREORDER functions called by Inspector *******/
bool VisitStepper::preorder(const IR::Node* node) {
    BUG("%1%: Unhandled node type: %2%", node, node->node_type_name());
}

bool VisitStepper::preorder(const IR::AssignmentStatement* assign) {

    if (!SymbolicEnv::isSymbolicValue(assign->right)) {
        return stepToSubexpr(assign->right, result, state,
                             [assign](const Continuation::Parameter* v) {
                                 auto* result = assign->clone();
                                 result->right = v->param;
                                 return result;
                             });
    }

    state.markVisited(assign);
    const auto* left = assign->left;
    const auto* leftType = left->type;

    // Resolve the type of the left-and assignment, if it is a type name.
    leftType = state.resolveType(leftType);

    // Although we typically expand structure assignments into individual member assignments using
    // the copyHeaders pass, some extern functions may return a list or struct expression. We can
    // not always expand these return values as we do with the expandLookahead pass.
    // Correspondingly, we need to retrieve the fields and set each member individually. This
    // assumes that all headers and structures have been flattened and no nesting is left.
    if (const auto* structType = leftType->to<IR::Type_StructLike>()) {
        const auto* listExpr = assign->right->checkedTo<IR::ListExpression>();

        std::vector<const IR::Member*> flatRefValids;
        auto flatRefFields = state.getFlatFields(left, structType, &flatRefValids);
        // First, complete the assignments for the data structure.
        for (size_t idx = 0; idx < flatRefFields.size(); ++idx) {
            const auto* leftFieldRef = flatRefFields[idx];
            state.set(leftFieldRef, listExpr->components[idx]);
        }
        // In case of a header, we also need to set the validity bits to true.
        for (const auto* headerValid : flatRefValids) {
            state.set(headerValid, IR::getBoolLiteral(true));
        }
    } else if (leftType->is<IR::Type_Base>()) {
        // Convert a path expression into the respective member.
        if (const auto* path = assign->left->to<IR::PathExpression>()) {
            left = state.convertPathExpr(path);
        }
        // Check that all members have the correct format. All members end with a pathExpression.
        checkMemberInvariant(left);
        state.set(left, assign->right);
    } else {
        TESTGEN_UNIMPLEMENTED("Unsupported assign type %1% node: %2%", leftType,
                              leftType->node_type_name());
    }

    state.popBody();
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::P4Parser* p4parser) {
    logStep(p4parser);

    auto* nextState = new VisitState(state);

    auto blockName = p4parser->getName().name;

    // Register and do target-specific initialization of the parser.
    const auto* constraint = startParser(p4parser, nextState);

    // Add trace events.
    nextState->add(new TraceEvent::ParserStart(p4parser, blockName));

    // Remove the invocation of the parser from the current body and push the resulting
    // continuation onto the continuation stack.
    nextState->popBody();

    auto handlers = getExceptionHandlers(p4parser, nextState->getBody(), nextState);
    nextState->pushCurrentContinuation(handlers);

    // Obtain the parser's namespace.
    const auto* ns = p4parser->to<IR::INamespace>();
    CHECK_NULL(ns);
    // Enter the parser's namespace.
    nextState->pushNamespace(ns);

    // Set the start state as the new body.
    const auto* startState = p4parser->states.getDeclaration<IR::ParserState>("start");
    std::vector<Continuation::Command> cmds;

    // Initialize parser-local declarations.
    for (const auto* decl : p4parser->parserLocals) {
        if (const auto* declVar = decl->to<IR::Declaration_Variable>()) {
            declareVariable(nextState, declVar);
        }
    }
    cmds.emplace_back(startState);
    nextState->replaceBody(Continuation::Body(cmds));

    result->emplace_back(constraint, state, nextState);
    return false;
}

bool VisitStepper::preorder(const IR::P4Control* p4control) {
    auto* nextState = new VisitState(state);

    auto blockName = p4control->getName().name;

    // Add trace events.
    std::stringstream controlName;
    controlName << "Control " << blockName << " start";
    nextState->add(new TraceEvent::Generic(controlName));

    // Set the emit buffer to be zero for the current control pipeline.
    nextState->resetEmitBuffer();

    // Obtain the control's namespace.
    const auto* ns = p4control->to<IR::INamespace>();
    CHECK_NULL(ns);
    // Enter the control's namespace.
    nextState->pushNamespace(ns);

    // Add control-local declarations.
    std::vector<Continuation::Command> cmds;
    for (const auto* decl : p4control->controlLocals) {
        if (const auto* declVar = decl->to<IR::Declaration_Variable>()) {
            declareVariable(nextState, declVar);
        }
    }
    // Add the body, if it is not empty
    if (p4control->body != nullptr) {
        cmds.emplace_back(p4control->body);
    }
    // Remove the invocation of the control from the current body and push the resulting
    // continuation onto the continuation stack.
    nextState->popBody();
    // Exit terminates the entire control block (only the control).
    std::map<Continuation::Exception, Continuation> handlers;
    handlers.emplace(Continuation::Exception::Exit, Continuation::Body({}));
    nextState->pushCurrentContinuation(handlers);
    // If the cmds are not empty, replace the body
    if (!cmds.empty()) {
        Continuation::Body newBody(cmds);
        nextState->replaceBody(newBody);
    }

    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::EmptyStatement* /*empty*/) {
    auto* nextState = new VisitState(state);
    nextState->popBody();
    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::IfStatement* ifStatement) {
    logStep(ifStatement);

    if (!SymbolicEnv::isSymbolicValue(ifStatement->condition)) {
        // Evaluate the condition.
        return stepToSubexpr(ifStatement->condition, result, state,
                             [ifStatement](const Continuation::Parameter* v) {
                                 auto* result = ifStatement->clone();
                                 result->condition = v->param;
                                 return result;
                             });
    }
    // If the condition of the if statement is tainted, the program counter is effectively tainted.
    // We insert a property that marks all execution as tainted until the if statement has
    // completed.
    // Because we may have nested taint, we need to check if we are already tainted.
    if (state.hasTaint(ifStatement->condition)) {
        auto* nextState = new VisitState(state);
        std::vector<Continuation::Command> cmds;
        auto currentTaint = state.getProperty<bool>("inUndefinedState");
        nextState->add(
            new TraceEvent::PreEvalExpression(ifStatement->condition, "Tainted If Statement"));
        cmds.emplace_back(Continuation::PropertyUpdate("inUndefinedState", true));
        cmds.emplace_back(ifStatement->ifTrue);
        if (ifStatement->ifFalse != nullptr) {
            cmds.emplace_back(ifStatement->ifFalse);
        }
        cmds.emplace_back(Continuation::PropertyUpdate("inUndefinedState", currentTaint));
        nextState->replaceTopBody(&cmds);
        result->emplace_back(nextState);
        return false;
    }
    // Handle case where a condition is true: proceed to a body.
    {
        auto* nextState = new VisitState(state);
        std::vector<Continuation::Command> cmds;
        nextState->add(
            new TraceEvent::PreEvalExpression(ifStatement->condition, "If Statement true"));
        cmds.emplace_back(ifStatement->ifTrue);
        nextState->replaceTopBody(&cmds);
        result->emplace_back(ifStatement->condition, state, nextState);
    }

    // Handle case for else body.
    {
        auto* negation = new IR::LNot(IR::Type::Boolean::get(), ifStatement->condition);
        auto* nextState = new VisitState(state);
        nextState->add(
            new TraceEvent::PreEvalExpression(ifStatement->condition, "If Statement false"));

        nextState->replaceTopBody((ifStatement->ifFalse == nullptr) ? new IR::BlockStatement()
                                                                    : ifStatement->ifFalse);
        result->emplace_back(negation, state, nextState);
    }

    return false;
}

bool VisitStepper::preorder(const IR::MethodCallStatement* methodCallStatement) {
    state.markVisited(methodCallStatement);
    state.popBody();
    const auto* type = methodCallStatement->methodCall->type;
    // do not push continuation for table application
    if (methodCallStatement->methodCall->method->type->is<IR::Type_Method>() &&
        methodCallStatement->methodCall->method->is<IR::PathExpression>()) {
        type = methodCallStatement->methodCall->type;
    } else if (state.getActionDecl(methodCallStatement->methodCall->method) != nullptr ||
               state.getTableType(methodCallStatement->methodCall->method) != nullptr) {
        type = IR::Type::Void::get();
    }
    state.pushCurrentContinuation(type);
    state.replaceBody(Continuation::Body({Continuation::Return(methodCallStatement->methodCall)}));
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::P4Program* /*program*/) {
    // Don't invoke logStep for the top-level program, as that would be overly verbose.

    // Get the initial constraints of the target. These constraints influence branch selection.
    boost::optional<const Constraint*> cond = programInfo.getTargetConstraints();

    // Have the target break apart the main declaration instance.
    auto* nextState = new VisitState(state);

    initializeVariablesFromTestCase(nextState, testCase);

    //CmdStepper::initializeTargetEnvironment(nextState);
    const auto* topLevelBlocks = programInfo.getPipelineSequence();

    // In between pipe lines we may drop packets or exit.
    // This segment inserts a special exception to deal with this case.
    // The drop exception just terminates execution completely.
    // We first pop the current body.
    nextState->popBody();
    // Then we insert the exception handlers.
    std::map<Continuation::Exception, Continuation> handlers;
    handlers.emplace(Continuation::Exception::Drop, Continuation::Body({}));
    handlers.emplace(Continuation::Exception::Exit, Continuation::Body({}));
    handlers.emplace(Continuation::Exception::Abort, Continuation::Body({}));
    nextState->pushCurrentContinuation(handlers);

    // After, we insert the program commands into the new body and push them to the top.
    Continuation::Body newBody(*topLevelBlocks);
    nextState->replaceBody(newBody);

    result->emplace_back(cond, state, nextState);
    return false;
}

bool VisitStepper::preorder(const IR::ParserState* parserState) {
    logStep(parserState);

    auto* nextState = new VisitState(state);

    nextState->add(new TraceEvent::ParserState(parserState));

    if (parserState->name == IR::ParserState::accept) {
        nextState->popContinuation();
        result->emplace_back(nextState);
        return false;
    }

    if (parserState->name == IR::ParserState::reject) {
        nextState->replaceTopBody(Continuation::Exception::Reject);
        result->emplace_back(nextState);
        return false;
    }

    // Push a new continuation that will take the next state as an argument and execute the state
    // as a command.
    // Create a parameter for the continuation we're about to build.
    const auto* v =
        Continuation::genParameter(IR::Type_State::get(), "nextState", state.getNamespaceContext());

    // Create the continuation itself.
    Continuation::Body kBody({v->param});
    Continuation k(v, kBody);
    nextState->pushContinuation(new VisitState::StackFrame(k, state.getNamespaceContext()));

    // Enter the parser state's namespace
    nextState->pushNamespace(parserState);

    // Replace the parser state with its non-declaration components, followed by the select
    // expression.
    std::vector<Continuation::Command> cmds;
    for (const auto* component : parserState->components) {
        if (component->is<IR::IDeclaration>() && !component->is<IR::Declaration_Variable>()) {
            continue;
        }
        if (const auto* declVar = component->to<IR::Declaration_Variable>()) {
            declareVariable(nextState, declVar);
        } else {
            cmds.emplace_back(component);
        }
    }

    // Add the next parser state(s) to the cmds
    cmds.emplace_back(Continuation::Return(parserState->selectExpression));

    nextState->replaceTopBody(&cmds);
    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::BlockStatement* block) {
    logStep(block);

    auto* nextState = new VisitState(state);
    std::vector<Continuation::Command> cmds;
    // Do not forget to add the namespace of the block statement.
    nextState->pushNamespace(block);
    // TODO (Fabian): Remove this? What is this for?
    for (const auto* component : block->components) {
        if (component->is<IR::IDeclaration>() && !component->is<IR::Declaration_Variable>()) {
            continue;
        }
        if (const auto* declVar = component->to<IR::Declaration_Variable>()) {
            declareVariable(nextState, declVar);
        } else {
            cmds.emplace_back(component);
        }
    }

    if (!cmds.empty()) {
        nextState->replaceTopBody(&cmds);
    } else {
        nextState->popBody();
    }

    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::ExitStatement* e) {
    logStep(e);
    auto* nextState = new VisitState(state);
    nextState->markVisited(e);
    nextState->replaceTopBody(Continuation::Exception::Exit);
    result->emplace_back(nextState);
    return false;
}

const Constraint* VisitStepper::startParser(const IR::P4Parser* parser, VisitState* nextState) {
    // Reset the parser cursor to zero.
    const auto* parserCursorVarType = VisitState::getPacketSizeVarType();

    // Constrain the input packet size to its maximum.
    const auto* boolType = IR::Type::Boolean::get();
    const Constraint* result =
        new IR::Leq(boolType, VisitState::getInputPacketSizeVar(),
                    IR::getConstant(parserCursorVarType, VisitState::getMaxPacketLength()));

    // Constrain the input packet size to be a multiple of 8 bits. Do this by constraining the
    // lowest three bits of the packet size to 0.
    const auto* threeBitType = IR::Type::Bits::get(3);
    result = new IR::LAnd(
        boolType, result,
        new IR::Equ(boolType,
                    new IR::Slice(threeBitType, VisitState::getInputPacketSizeVar(),
                                  IR::getConstant(parserCursorVarType, 2),
                                  IR::getConstant(parserCursorVarType, 0)),
                    IR::getConstant(threeBitType, 0)));

    // Call the implementation for the specific target.
    // If we get a constraint back, add it to the result.
    if (auto constraintOpt = startParser_impl(parser, nextState)) {
        result = new IR::LAnd(boolType, result, *constraintOpt);
    }

    return result;
}

IR::SwitchStatement* VisitStepper::replaceSwitchLabels(const IR::SwitchStatement* switchStatement) {
    const auto* member = switchStatement->expression->to<IR::Member>();
    BUG_CHECK(member != nullptr && member->member.name == IR::Type_Table::action_run,
              "Invalid format of %1% for action_run", switchStatement->expression);
    const auto* methodCall = member->expr->to<IR::MethodCallExpression>();
    BUG_CHECK(methodCall, "Invalid format of %1% for action_run", member->expr);
    const auto* tableCall = methodCall->method->to<IR::Member>();
    BUG_CHECK(tableCall, "Invalid format of %1% for action_run", methodCall->method);
    const auto* table = state.getTableType(methodCall->method->to<IR::Member>());
    CHECK_NULL(table);
    auto actionVar = TableVisitor::getTableActionVar(table);
    IR::Vector<IR::SwitchCase> newCases;
    std::map<cstring, int> actionsIds;
    for (size_t index = 0; index < table->getActionList()->size(); index++) {
        actionsIds.emplace(table->getActionList()->actionList.at(index)->getName().toString(),
                           index);
    }
    for (const auto* switchCase : switchStatement->cases) {
        auto* newSwitchCase = switchCase->clone();
        // Do not replace default expression labels.
        if (!newSwitchCase->label->is<IR::DefaultExpression>()) {
            newSwitchCase->label =
                IR::getConstant(actionVar->type, actionsIds[switchCase->label->toString()]);
        }
        newCases.push_back(newSwitchCase);
    }
    auto* newSwitch = switchStatement->clone();
    newSwitch->cases = newCases;
    return newSwitch;
}

bool VisitStepper::preorder(const IR::SwitchStatement* switchStatement) {
    logStep(switchStatement);

    if (!SymbolicEnv::isSymbolicValue(switchStatement->expression)) {
        // Evaluate the keyset in the first select case.
        return stepToSubexpr(
            switchStatement->expression, result, state,
            [switchStatement, this](const Continuation::Parameter* v) {
                if (!switchStatement->expression->type->is<IR::Type_ActionEnum>()) {
                    BUG("Only switch statements with action_run as expression are supported.");
                }
                // If the switch statement has table action as expression, replace
                // the case labels with indices.
                auto* newSwitch = replaceSwitchLabels(switchStatement);
                newSwitch->expression = v->param;
                return newSwitch;
            });
    }
    // After we have executed, we simple pick the index that matches with the returned constant.
    auto* nextState = new VisitState(state);
    std::vector<Continuation::Command> cmds;
    // If the switch expression is tainted, we can not predict which case will be chosen. We taint
    // the program counter and execute all of the statements.
    if (state.hasTaint(switchStatement->expression)) {
        auto currentTaint = state.getProperty<bool>("inUndefinedState");
        cmds.emplace_back(Continuation::PropertyUpdate("inUndefinedState", true));
        for (const auto* switchCase : switchStatement->cases) {
            if (switchCase->statement != nullptr) {
                cmds.emplace_back(switchCase->statement);
            }
        }
        cmds.emplace_back(Continuation::PropertyUpdate("inUndefinedState", currentTaint));
    } else {
        // Otherwise, we pick the switch statement case in a normal fashion.

        bool hasMatched = false;
        for (const auto* switchCase : switchStatement->cases) {
            // We have either matched already, or still need to match.
            hasMatched = hasMatched || switchStatement->expression->equiv(*switchCase->label);
            // Nothing to do with this statement. Fall through to the next case.
            if (switchCase->statement == nullptr) {
                continue;
            }
            // If any of the values in the match list hits, execute the switch case block.
            if (hasMatched) {
                cmds.emplace_back(switchCase->statement);
                // If the statement is a block, we do not fall through and terminate execution.
                if (switchCase->statement->is<IR::BlockStatement>()) {
                    break;
                }
            }
            // The default label must be last. Always break here.
            if (switchCase->label->is<IR::DefaultExpression>()) {
                cmds.emplace_back(switchCase->statement);
                break;
            }
        }
    }

    BUG_CHECK(!cmds.empty(), "Switch statements should have at least one case (default).");
    nextState->replaceTopBody(&cmds);
    result->emplace_back(nextState);

    return false;
}

bool VisitStepper::preorder(const IR::BoolLiteral* boolLiteral) {
    logStep(boolLiteral);
    state.popContinuation(boolLiteral);
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::Constant* constant) {
    logStep(constant);
    state.popContinuation(constant);
    result->emplace_back(&state);
    return false;
}

void VisitStepper::handleHitMissActionRun(const IR::Member* member) {
    auto* nextState = new VisitState(state);
    std::vector<Continuation::Command> replacements;
    const auto* method = member->expr->to<IR::MethodCallExpression>();
    BUG_CHECK(method->method->is<IR::Member>(), "Method apply has unexpected format: %1%", method);
    replacements.emplace_back(new IR::MethodCallStatement(method));
    const auto* methodName = method->method->to<IR::Member>();
    const auto* table = state.getTableType(methodName);
    CHECK_NULL(table);
    if (member->member.name == IR::Type_Table::hit) {
        replacements.emplace_back(Continuation::Return(TableVisitor::getTableHitVar(table)));
    } else if (member->member.name == IR::Type_Table::miss) {
        replacements.emplace_back(
            Continuation::Return(new IR::LNot(TableVisitor::getTableHitVar(table))));
    } else {
        replacements.emplace_back(Continuation::Return(TableVisitor::getTableActionVar(table)));
    }
    nextState->replaceTopBody(&replacements);
    result->emplace_back(nextState);
}

bool VisitStepper::preorder(const IR::Member* member) {
    logStep(member);

    if (member->expr->is<IR::MethodCallExpression>() &&
        (member->member.name == IR::Type_Table::hit ||
         member->member.name == IR::Type_Table::miss ||
         member->member.name == IR::Type_Table::action_run)) {
        handleHitMissActionRun(member);
        return false;
    }

    // TODO: Do we need to handle non-numeric, non-boolean expressions?
    BUG_CHECK(member->type->is<IR::Type::Bits>() || member->type->is<IR::Type::Boolean>() ||
                  member->type->is<IR::Extracted_Varbits>(),
              "Non-numeric, non-boolean member expression: %1% Type: %2%", member,
              member->type->node_type_name());

    // Check our assumption that this is a chain of member expressions terminating in a
    // PathExpression.
    checkMemberInvariant(member);

    // This case may happen when we directly assign taint, that is not bound in the symbolic
    // environment to an expression.
    const IR::Node* expr = member;
    if (!SymbolicEnv::isSymbolicValue(member)) {
        expr = state.get(member);
    }

    state.popContinuation(expr);
    result->emplace_back(&state);
    return false;
}

void VisitStepper::evalActionCall(const IR::P4Action* action, const IR::MethodCallExpression* call) {
    const auto* actionNameSpace = action->to<IR::INamespace>();
    BUG_CHECK(actionNameSpace, "Does not instantiate an INamespace: %1%", actionNameSpace);
    auto* nextState = new VisitState(state);
    // If the action has arguments, these are usually directionless control plane input.
    // We introduce a zombie variable that takes the argument value. This value is either
    // provided by a constant entry or synthesized by us.
    for (size_t argIdx = 0; argIdx < call->arguments->size(); ++argIdx) {
        const auto& parameters = action->parameters;
        const auto* param = parameters->getParameter(argIdx);
        const auto* paramType = param->type;
        const auto paramName = param->name;
        BUG_CHECK(param->direction == IR::Direction::None,
                  "%1%: Only directionless action parameters are supported at this point. ",
                  action);
        const auto& tableActionDataVar = Utils::getZombieVar(paramType, 0, paramName);
        const auto* curArg = call->arguments->at(argIdx)->expression;
        nextState->set(tableActionDataVar, curArg);
    }
    nextState->replaceTopBody(action->body);
    // Enter the action's namespace.
    nextState->pushNamespace(actionNameSpace);
    result->emplace_back(nextState);
}

bool VisitStepper::preorder(const IR::MethodCallExpression* call) {
    logStep(call);
    // A method call expression represents an invocation of an action, a table, an extern, or
    // setValid/setInvalid.

    // Handle method calls. These are either table invocations or extern calls.
    if (call->method->type->is<IR::Type_Method>()) {
        if (const auto* path = call->method->to<IR::PathExpression>()) {
            // Case where call->method is a PathExpression expression.
            const auto* member = new IR::Member(
                call->method->type,
                new IR::PathExpression(new IR::Type_Extern("*method"), new IR::Path("*method")),
                path->path->name);
            evalExternMethodCall(call, member->expr, member->member, call->arguments, state);
            return false;
        }

        if (const auto* method = call->method->to<IR::Member>()) {
            // Case where call->method is a Member expression. For table invocations, the
            // qualifier of the member determines the table being invoked. For extern calls,
            // the qualifier determines the extern object containing the method being invoked.
            BUG_CHECK(method->expr, "Method call has unexpected format: %1%", call);

            // Handle table calls.
            if (const auto* table = state.getTableType(method)) {
                auto* nextState = new VisitState(state);
                nextState->replaceTopBody(Continuation::Return(table));
                result->emplace_back(nextState);
                return false;
            }

            // Handle extern calls. They may also be of Type_SpecializedCanonical.
            if (method->expr->type->is<IR::Type_Extern>() ||
                method->expr->type->is<IR::Type_SpecializedCanonical>()) {
                evalExternMethodCall(call, method->expr, method->member, call->arguments, state);
                return false;
            }

            // Handle calls to header methods.
            if (method->expr->type->is<IR::Type_Header>() ||
                method->expr->type->is<IR::Type_HeaderUnion>()) {
                if (method->member == "isValid") {
                    return stepGetHeaderValidity(method->expr);
                }

                if (method->member == "setInvalid") {
                    return stepSetHeaderValidity(method->expr, false);
                }

                if (method->member == "setValid") {
                    return stepSetHeaderValidity(method->expr, true);
                }

                BUG("Unknown method call on header instance: %1%", call);
            }

            if (method->expr->type->is<IR::Type_Stack>()) {
                if (method->member == "push_front") {
                    return stepStackPushPopFront(method->expr, call->arguments);
                }

                if (method->member == "pop_front") {
                    return stepStackPushPopFront(method->expr, call->arguments, false);
                }

                BUG("Unknown method call on stack instance: %1%", call);
            }

            BUG("Unknown method member expression: %1% of type %2%", method->expr,
                method->expr->type);
        }

        BUG("Unknown method call: %1% of type %2%", call->method, call->method->node_type_name());
        // Handle action calls. Actions are called by tables and are not inlined, unlike
        // functions.
    } else if (const auto* action = state.getActionDecl(call->method)) {
        evalActionCall(action, call);
        return false;
    }

    BUG("Unknown method call expression: %1%", call);
}

bool VisitStepper::preorder(const IR::P4Table* table) {
    // Delegate to the tableStepper.
    TableVisitor tableVisitor(this, table, testCase);

    return tableVisitor.eval();
}

bool VisitStepper::preorder(const IR::Mux* mux) {
    logStep(mux);

    if (!SymbolicEnv::isSymbolicValue(mux->e0)) {
        return stepToSubexpr(mux->e0, result, state, [mux](const Continuation::Parameter* v) {
            auto* result = mux->clone();
            result->e0 = v->param;
            return Continuation::Return(result);
        });
    }
    // If the Mux condition  is tainted, just return a taint constant.
    if (state.hasTaint(mux->e0)) {
        auto* nextState = new VisitState(state);
        nextState->replaceTopBody(
            Continuation::Return(programInfo.createTargetUninitialized(mux->type, true)));
        result->emplace_back(nextState);
        return false;
    }
    // A list of path conditions paired with the resulting expression for each branch.
    std::list<std::pair<const IR::Expression*, const IR::Expression*>> branches = {
        {mux->e0, mux->e1},
        {new IR::LNot(mux->e0->type, mux->e0), mux->e2},
    };

    for (const auto& entry : branches) {
        const auto* cond = entry.first;
        const auto* expr = entry.second;

        auto* nextState = new VisitState(state);
        nextState->replaceTopBody(Continuation::Return(expr));
        result->emplace_back(cond, state, nextState);
    }

    return false;
}

bool VisitStepper::preorder(const IR::PathExpression* pathExpression) {
    logStep(pathExpression);

    // If we are referencing a parser state, step into the state.
    const auto* decl = state.findDecl(pathExpression)->getNode();
    if (decl->is<IR::ParserState>()) {
        state.popContinuation(decl);
        result->emplace_back(&state);
        return false;
    }

    auto* nextState = new VisitState(state);
    // ValueSets can be declared in parsers and are usually set by the control plane.
    // We simply return the contained valueSet.
    if (const auto* valueSet = decl->to<IR::P4ValueSet>()) {
        nextState->replaceTopBody(Continuation::Return(valueSet));
        result->emplace_back(nextState);
        return false;
    }
    // Otherwise convert the path expression into a qualified member and return it.
    auto pathRef = nextState->convertPathExpr(pathExpression);
    nextState->replaceTopBody(Continuation::Return(pathRef));
    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::P4ValueSet* valueSet) {
    logStep(valueSet);

    auto vsSize = valueSet->size->checkedTo<IR::Constant>()->value;
    auto* nextState = new VisitState(state);
    IR::Vector<IR::Expression> components;
    // TODO: Fill components with values when we have an API.
    const auto* pvsType = valueSet->elementType;
    pvsType = state.resolveType(pvsType);
    TESTGEN_UNIMPLEMENTED("Value Set not yet fully implemented");

    nextState->popBody();
    result->emplace_back(nextState);
    return false;
}

bool VisitStepper::preorder(const IR::Operation_Binary* binary) {
    logStep(binary);

    if (!SymbolicEnv::isSymbolicValue(binary->left)) {
        return stepToSubexpr(binary->left, result, state,
                             [binary](const Continuation::Parameter* v) {
                                 auto* result = binary->clone();
                                 result->left = v->param;
                                 return Continuation::Return(result);
                             });
    }

    if (!SymbolicEnv::isSymbolicValue(binary->right)) {
        return stepToSubexpr(binary->right, result, state,
                             [binary](const Continuation::Parameter* v) {
                                 auto* result = binary->clone();
                                 result->right = v->param;
                                 return Continuation::Return(result);
                             });
    }

    // Handle saturating arithmetic expressions by translating them into Mux expressions.
    if (P4::SaturationElim::isSaturationOperation(binary)) {
        auto* nextState = new VisitState(state);
        nextState->replaceTopBody(Continuation::Return(P4::SaturationElim::eliminate(binary)));
        result->emplace_back(nextState);
        return false;
    }

    state.popContinuation(binary);
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::Operation_Unary* unary) {
    logStep(unary);

    if (!SymbolicEnv::isSymbolicValue(unary->expr)) {
        return stepToSubexpr(unary->expr, result, state, [unary](const Continuation::Parameter* v) {
            auto* result = unary->clone();
            result->expr = v->param;
            return Continuation::Return(result);
        });
    }

    state.popContinuation(unary);
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::SelectExpression* selectExpression) {
    logStep(selectExpression);

    // If there are no select cases, then the select expression has failed to match on anything.
    // Delegate to stepNoMatch.
    if (selectExpression->selectCases.empty()) {
        stepNoMatch();
        return false;
    }

    const auto* selectCase = selectExpression->selectCases.at(0);
    // Getting P4ValueSet from PathExpression , to highlight a particular case of processing
    // P4ValueSet.
    if (const auto* pathExpr = selectCase->keyset->to<IR::PathExpression>()) {
        const auto* decl = state.findDecl(pathExpr)->getNode();
        // Until we have test support for value set, we simply remove them from the match list.
        // TODO: Implement value sets.
        if (decl->is<IR::P4ValueSet>()) {
            auto* newSelectExpression = selectExpression->clone();
            newSelectExpression->selectCases.erase(newSelectExpression->selectCases.begin());
            auto* nextState = new VisitState(state);
            nextState->replaceTopBody(Continuation::Return(newSelectExpression));
            result->emplace_back(nextState);
            return false;
        }
    }

    if (!SymbolicEnv::isSymbolicValue(selectCase->keyset)) {
        // Evaluate the keyset in the first select case.
        return stepToSubexpr(selectCase->keyset, result, state,
                             [selectExpression, selectCase](const Continuation::Parameter* v) {
                                 auto* newSelectCase = selectCase->clone();
                                 newSelectCase->keyset = v->param;

                                 auto* result = selectExpression->clone();
                                 result->selectCases[0] = newSelectCase;
                                 return Continuation::Return(result);
                             });
    }

    if (!SymbolicEnv::isSymbolicValue(selectExpression->select)) {
        // Evaluate the expression being selected.
        return stepToListSubexpr(selectExpression->select, result, state,
                                 [selectExpression](const IR::ListExpression* listExpr) {
                                     auto* result = selectExpression->clone();
                                     result->select = listExpr;
                                     return Continuation::Return(result);
                                 });
    }

    // Handle case where the first select case matches: proceed to the next parser state,
    // guarded by its path condition.
    const auto* equality = GenEq::equate(selectExpression->select, selectCase->keyset);

    // TODO: Implement the taint case for select expressions.
    // In the worst case, this means the entire parser is tainted.
    if (state.hasTaint(equality)) {
        TESTGEN_UNIMPLEMENTED(
            "The SelectExpression %1% is trying to match on a tainted key set."
            " This means it is matching on uninitialized data."
            " P4Testgen currently does not support this case.",
            selectExpression);
    }

    boost::optional<bool> isEqual;
    if (selectExpression->select->components.size() == 1 &&
            dynamic_cast<const IR::Constant*>(selectExpression->select->components[0]) != nullptr) {

        if (dynamic_cast<const IR::Constant*>(selectCase->keyset) != nullptr) {
            // Compare two constants directly
            isEqual = selectExpression->select->components[0]->checkedTo<IR::Constant>()->value ==
                selectCase->keyset->checkedTo<IR::Constant>()->value;

        } else if (dynamic_cast<const IR::DefaultExpression*>(selectCase->keyset) != nullptr) {
            // Select the statement.
            auto* nextState = new VisitState(state);
            nextState->replaceTopBody(Continuation::Return(selectCase->state));
            result->emplace_back(nextState);
            return false;
        }
    }

    if (isEqual == boost::none || isEqual == true) {
        auto* nextState = new VisitState(state);
        nextState->replaceTopBody(Continuation::Return(selectCase->state));
        result->emplace_back(equality, state, nextState);
    }

    // Handle case where the first select case doesn't match.
    if (isEqual == boost::none || isEqual == false) {
        auto* inequality = new IR::LNot(IR::Type::Boolean::get(), equality);
        auto* nextState = new VisitState(state);

        // Remove the first select case from the select expression.
        auto* nextSelectExpression = selectExpression->clone();
        nextSelectExpression->selectCases.erase(nextSelectExpression->selectCases.begin());

        nextState->replaceTopBody(Continuation::Return(nextSelectExpression));
        result->emplace_back(inequality, state, nextState);
    }

    return false;
}

bool VisitStepper::preorder(const IR::ListExpression* listExpression) {
    if (!SymbolicEnv::isSymbolicValue(listExpression)) {
        // Evaluate the expression being selected.
        return stepToListSubexpr(listExpression, result, state,
                                 [listExpression](const IR::ListExpression* listExpr) {
                                     const auto* result = listExpression->clone();
                                     result = listExpr;
                                     return Continuation::Return(result);
                                 });
    }
    state.popContinuation(listExpression);
    result->emplace_back(&state);
    return false;
}


bool VisitStepper::preorder(const IR::StructExpression* structExpression) {
    if (!SymbolicEnv::isSymbolicValue(structExpression)) {
        // Evaluate the expression being selected.
        return stepToStructSubexpr(structExpression, result, state,
                                   [structExpression](const IR::StructExpression* structExpr) {
                                       const auto* result = structExpression->clone();
                                       result = structExpr;
                                       return Continuation::Return(result);
                                   });
    }
    state.popContinuation(structExpression);
    result->emplace_back(&state);
    return false;
}

bool VisitStepper::preorder(const IR::Slice* slice) {
    logStep(slice);

    if (!SymbolicEnv::isSymbolicValue(slice->e0)) {
        return stepToSubexpr(slice->e0, result, state, [slice](const Continuation::Parameter* v) {
            auto* result = slice->clone();
            result->e0 = v->param;
            return Continuation::Return(result);
        });
    }

    // Check that slices values are constants.
    slice->e1->checkedTo<IR::Constant>();
    slice->e2->checkedTo<IR::Constant>();

    state.popContinuation(slice);
    result->emplace_back(&state);
    return false;
}

void VisitStepper::stepNoMatch() { stepToException(Continuation::Exception::NoMatch); }

}  // namespace P4Testgen

}  // namespace P4Tools

