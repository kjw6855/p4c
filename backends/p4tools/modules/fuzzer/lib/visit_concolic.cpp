#include "backends/p4tools/modules/fuzzer/lib/visit_concolic.h"

#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "backends/p4tools/common/lib/formulae.h"
#include "backends/p4tools/common/lib/model.h"
#include "ir/id.h"
#include "lib/exceptions.h"
#include "lib/null.h"

#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"

namespace P4Tools {

namespace P4Testgen {

bool VisitConcolicMethodImpls::matches(const std::vector<cstring>& paramNames,
                                  const IR::Vector<IR::Argument>* args) {
    CHECK_NULL(args);

    // Number of parameters should match the number of arguments.
    if (paramNames.size() != args->size()) {
        return false;
    }
    // Any named argument should match the name of the corresponding parameter.
    for (size_t idx = 0; idx < paramNames.size(); idx++) {
        const auto& paramName = paramNames.at(idx);
        const auto& arg = args->at(idx);

        if (arg->name.name == nullptr) {
            continue;
        }
        if (paramName != arg->name.name) {
            return false;
        }
    }

    return true;
}

bool VisitConcolicMethodImpls::exec(cstring qualifiedMethodName, const IR::ConcolicVariable* var,
                               const VisitState& state, const Model* completedModel,
                               VisitConcolicVariableMap* resolvedVisitConcolicVariables) const {
    if (impls.count(qualifiedMethodName) == 0) {
        return false;
    }

    const auto* args = var->arguments;

    const auto& submap = impls.at(qualifiedMethodName);
    if (submap.count(args->size()) == 0) {
        return false;
    }

    // Find matching methods: if any arguments are named, then the parameter name must match.
    boost::optional<MethodImpl> matchingImpl;
    for (const auto& pair : submap.at(args->size())) {
        const auto& paramNames = pair.first;
        const auto& methodImpl = pair.second;

        if (matches(paramNames, args)) {
            BUG_CHECK(!matchingImpl, "Ambiguous extern method call: %1%", qualifiedMethodName);
            matchingImpl = methodImpl;
        }
    }

    if (!matchingImpl) {
        return false;
    }
    (*matchingImpl)(qualifiedMethodName, var, state, completedModel, resolvedVisitConcolicVariables);
    return true;
}

void VisitConcolicMethodImpls::add(const ImplList& inputImplList) {
    for (const auto& implSpec : inputImplList) {
        cstring name;
        std::vector<cstring> paramNames;
        MethodImpl impl;
        std::tie(name, paramNames, impl) = implSpec;

        auto& tmpImplList = impls[name][paramNames.size()];

        // Make sure that we have at most one implementation for each set of parameter names.
        // This is a quadratic-time algorithm, but should be fine, since we expect the number of
        // overloads to be small in practice.
        for (auto& pair : tmpImplList) {
            BUG_CHECK(pair.first != paramNames, "Multiple implementations of %1%(%2%)", name,
                      paramNames);
        }

        tmpImplList.emplace_back(paramNames, impl);
    }
}

VisitConcolicMethodImpls::VisitConcolicMethodImpls(const ImplList& implList) { add(implList); }

bool VisitConcolicResolver::preorder(const IR::ConcolicVariable* var) {
    cstring concolicMethodName = var->concolicMethodName;
    // Convert the concolic member variable to a state variable.
    StateVariable concolicVarName = var->concolicMember;
    auto concolicReplacment = resolvedVisitConcolicVariables.find(concolicVarName);
    if (concolicReplacment == resolvedVisitConcolicVariables.end()) {
        bool found = concolicMethodImpls->exec(concolicMethodName, var, state, completedModel,
                                               &resolvedVisitConcolicVariables);
        BUG_CHECK(found, "Unknown or unimplemented concolic method: %1%", concolicMethodName);
    }
    return false;
}

const VisitConcolicVariableMap* VisitConcolicResolver::getResolvedVisitConcolicVariables() const {
    return &resolvedVisitConcolicVariables;
}

VisitConcolicResolver::VisitConcolicResolver(const Model* completedModel, const VisitState& state,
                                   const VisitConcolicMethodImpls* concolicMethodImpls)
    : state(state), completedModel(completedModel), concolicMethodImpls(concolicMethodImpls) {
    visitDagOnce = false;
}

VisitConcolicResolver::VisitConcolicResolver(const Model* completedModel, const VisitState& state,
                                   const VisitConcolicMethodImpls* concolicMethodImpls,
                                   VisitConcolicVariableMap resolvedVisitConcolicVariables)
    : state(state),
      completedModel(completedModel),
      resolvedVisitConcolicVariables(std::move(resolvedVisitConcolicVariables)),
      concolicMethodImpls(concolicMethodImpls) {
    visitDagOnce = false;
}

static const VisitConcolicMethodImpls::ImplList coreVisitConcolicMethodImpls({});

const VisitConcolicMethodImpls::ImplList* VisitConcolic::getCoreVisitConcolicMethodImpls() {
    return &coreVisitConcolicMethodImpls;
}

}  // namespace P4Testgen

}  // namespace P4Tools
