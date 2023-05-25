#include "backends/p4tools/modules/fuzzer/lib/final_visit_state.h"

#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/trace_events.h"

#include "backends/p4tools/modules/fuzzer/lib/execution_state.h"

namespace P4Tools {

namespace P4Testgen {

FinalVisitState::FinalVisitState(const VisitState& inputState)
    : state(VisitState(inputState)),
      completedModel(completeModel(inputState)) {
    for (const auto& event : inputState.getTrace()) {
        trace.emplace_back(event->evaluate(completedModel));
    }
    visitedStatements = inputState.getVisited();
}

Model FinalVisitState::completeModel(const VisitState& visitState) {
    auto* model = new Model();
    // Complete the model based on the symbolic environment.
    auto* completedModel = visitState.getSymbolicEnv().complete(*model);

    // Also complete all the zombies that were collected in this state.
    const auto& zombies = visitState.getZombies();
    completedModel->complete(zombies);

    // Now that the models initial values are completed evaluate the values that
    // are part of the constraints that have been added to the solver.
    auto* evaluatedModel = visitState.getSymbolicEnv().evaluate(*completedModel);

    for (const auto& event : visitState.getTrace()) {
        event->complete(evaluatedModel);
    }

    return *evaluatedModel;
}

const Model* FinalVisitState::getCompletedModel() const { return &completedModel; }

const VisitState* FinalVisitState::getVisitState() const { return &state; }

const std::vector<gsl::not_null<const TraceEvent*>>* FinalVisitState::getTraces() const {
    return &trace;
}

const std::vector<const IR::Statement*>& FinalVisitState::getVisited() const {
    return visitedStatements;
}

}  // namespace P4Testgen

}  // namespace P4Tools
