#include "backends/p4tools/modules/testgen/lib/final_visit_state.h"

#include <list>
#include <utility>
#include <variant>
#include <vector>

#include <boost/container/vector.hpp>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "backends/p4tools/common/lib/util.h"
#include "frontends/p4/optimizeExpressions.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "lib/error.h"
#include "lib/null.h"

#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/packet_vars.h"

namespace P4Tools::P4Testgen {

FinalVisitState::FinalVisitState(const ExecutionState &finalState)
    : state(finalState),
      completedModel(completeModel(finalState, new Model(*new SymbolicMapping()))) {
    for (const auto &event : finalState.getTrace()) {
        trace.emplace_back(*event.get().evaluate(completedModel));
    }
}

FinalVisitState::FinalVisitState(const ExecutionState &finalState,
                       const Model &completedModel)
    : state(finalState), completedModel(completedModel) {
    for (const auto &event : finalState.getTrace()) {
        trace.emplace_back(*event.get().evaluate(completedModel));
    }
}

void FinalVisitState::calculatePayload(const ExecutionState &executionState, Model &evaluatedModel) {
    const auto &packetBitSizeVar = ExecutionState::getInputPacketSizeVar();
    const auto *payloadSizeConst = evaluatedModel.evaluate(packetBitSizeVar);
    int calculatedPacketSize = IR::getIntFromLiteral(payloadSizeConst);
    const auto *inputPacketExpr = executionState.getInputPacket();
    int payloadSize = calculatedPacketSize - inputPacketExpr->type->width_bits();
    if (payloadSize > 0) {
        const auto *payloadType = IR::getBitType(payloadSize);
        const IR::Expression *payloadExpr = evaluatedModel.get(&PacketVars::PAYLOAD_LABEL, false);
        if (payloadExpr == nullptr) {
            payloadExpr = Utils::getRandConstantForType(payloadType);
            evaluatedModel.emplace(&PacketVars::PAYLOAD_LABEL, payloadExpr);
        }
    }
}

Model &FinalVisitState::completeModel(const ExecutionState &finalState, const Model *model,
                                 bool postProcess) {

    // Complete the model based on the symbolic environment.
    auto *completedModel = finalState.getSymbolicEnv().complete(*model);

    // Also complete all the symbolic variables that were collected in this state.
    const auto &symbolicVars = finalState.getSymbolicVariables();
    completedModel->complete(symbolicVars);

    // Now that the models initial values are completed evaluate the values that
    // are part of the constraints that have been added to the solver.
    auto *evaluatedModel = finalState.getSymbolicEnv().evaluate(*completedModel);

    if (postProcess) {
        // Append a payload, if requested.
        calculatePayload(finalState, *evaluatedModel);
    }
    for (const auto &event : finalState.getTrace()) {
        event.get().complete(evaluatedModel);
    }

    return *evaluatedModel;
}

std::optional<std::reference_wrapper<const FinalVisitState>> FinalVisitState::computeConcolicState(
    const ConcolicVariableMap &resolvedConcolicVariables) const {
    // If there are no new concolic variables, there is nothing to do.
    if (resolvedConcolicVariables.empty()) {
        return *this;
    }
    std::vector<const Constraint *> asserts = state.get().getPathConstraint();

    auto *symbolicMaps = new SymbolicMapping();

    for (const auto &resolvedConcolicVariable : resolvedConcolicVariables) {
        const auto &concolicVariable = resolvedConcolicVariable.first;
        const auto *concolicAssignment = resolvedConcolicVariable.second;
        const IR::Expression *pathConstraint = nullptr;

        // We need to differentiate between state variables and expressions here.
        if (std::holds_alternative<IR::ConcolicVariable>(concolicVariable)) {
            pathConstraint = new IR::Equ(std::get<IR::ConcolicVariable>(concolicVariable).clone(),
                                         concolicAssignment);
        } else if (std::holds_alternative<const IR::Expression *>(concolicVariable)) {
            pathConstraint =
                new IR::Equ(std::get<const IR::Expression *>(concolicVariable), concolicAssignment);
        }
        CHECK_NULL(pathConstraint);
        pathConstraint = state.get().getSymbolicEnv().subst(pathConstraint);
        pathConstraint = P4::optimizeExpression(pathConstraint);
        // Put SymbolicVariables here!!!
        if (dynamic_cast<const IR::Literal*>(pathConstraint) == nullptr &&
                dynamic_cast<const IR::Equ*>(pathConstraint) != nullptr) {
            if (std::holds_alternative<IR::ConcolicVariable>(concolicVariable)) {
                symbolicMaps->emplace(std::get<IR::ConcolicVariable>(concolicVariable).clone(),
                        concolicAssignment);
            }
        }

        //asserts.push_back(pathConstraint);
    }
#if 0
    auto solverResult = solver.get().checkSat(asserts);
    if (!solverResult) {
        ::warning("Timed out trying to solve this concolic execution path.");
        return std::nullopt;
    }

    if (!*solverResult) {
        ::warning("Concolic constraints for this path are unsatisfiable.");
        return std::nullopt;
    }
#endif

    auto &model = completeModel(state, new Model(*symbolicMaps), false);
    //auto &model = completeModel(state, new Model(solver.get().getSymbolicMapping()), false);
    /// Transfer any derived variables from that are missing  in this model.
    /// Do NOT update any variables that already exist.
    for (const auto &varTuple : completedModel) {
        model.emplace(varTuple.first, varTuple.second);
    }
    return *new FinalVisitState(state, model);
}

const Model *FinalVisitState::getCompletedModel() const { return &completedModel; }

const ExecutionState *FinalVisitState::getExecutionState() const { return &state.get(); }

const std::vector<std::reference_wrapper<const TraceEvent>> *FinalVisitState::getTraces() const {
    return &trace;
}

const P4::Coverage::CoverageSet &FinalVisitState::getVisited() const { return state.get().getVisited(); }
}  // namespace P4Tools::P4Testgen
