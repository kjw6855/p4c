#include "backends/p4tools/modules/testgen/lib/visit_concolic.h"

#include <optional>
#include <utility>
#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "ir/id.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/testgen/lib/concolic.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"

namespace P4Tools::P4Testgen {

bool VisitConcolicResolver::preorder(const IR::ConcolicVariable *var) {
    auto concolicMethodName = var->concolicMethodName;
    // Convert the concolic member variable to a state variable.
    auto concolicReplacment = resolvedConcolicVariables.find(*var);
    if (concolicReplacment == resolvedConcolicVariables.end()) {
        bool found = concolicMethodImpls.exec(concolicMethodName, var, state, completedModel,
                                              &resolvedConcolicVariables);
        BUG_CHECK(found, "Unknown or unimplemented concolic method: %1%", concolicMethodName);
    }
    return false;
}

const ConcolicVariableMap *VisitConcolicResolver::getResolvedConcolicVariables() const {
    return &resolvedConcolicVariables;
}

VisitConcolicResolver::VisitConcolicResolver(const Model &completedModel, const ExecutionState &state,
                                   const ConcolicMethodImpls &concolicMethodImpls)
    : state(state), completedModel(completedModel), concolicMethodImpls(concolicMethodImpls) {
    visitDagOnce = false;
}

VisitConcolicResolver::VisitConcolicResolver(const Model &completedModel, const ExecutionState &state,
                                   const ConcolicMethodImpls &concolicMethodImpls,
                                   ConcolicVariableMap resolvedConcolicVariables)
    : state(state),
      completedModel(completedModel),
      resolvedConcolicVariables(std::move(resolvedConcolicVariables)),
      concolicMethodImpls(concolicMethodImpls) {
    visitDagOnce = false;
}

}  // namespace P4Tools::P4Testgen
