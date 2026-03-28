#include "frontends/p4/metrics/actionParameterMetrics.h"

namespace P4 {

unsigned ActionParameterMetricsPass::paramSize(const IR::Parameter *param) {
    if (param == nullptr) return 0;

    const IR::Type *currentType = param->type;
    while (currentType != nullptr) {
        if (auto bitType = currentType->to<IR::Type_Bits>())
            return bitType->width_bits();
        else if (currentType->is<IR::Type_Boolean>())
            return 1;
        else if (currentType->to<IR::Type_Error>())
            return 32u;  // Common default
        else if (currentType->to<IR::Type_Enum>())
            return 32u;  // Target-dependent, default to 32 bits
        else if (auto serEnum = currentType->to<IR::Type_SerEnum>())
            return serEnum->type->width_bits();

        // Unwrap the type if it's a type definition wrapper
        if (auto tt = currentType->to<IR::Type_Type>())
            currentType = tt->type;
        else if (auto nt = currentType->to<IR::Type_Newtype>())
            currentType = typeMap->getType(nt->type, true);
        else if (auto td = currentType->to<IR::Type_Typedef>())
            currentType = typeMap->getType(td->type, true);
        else
            break;
    }

    return 0;  // Unsupported type
}

void ActionParameterMetricsPass::postorder(const IR::P4Action *action) {
    cstring actionName = action->getName();
    metrics.numActions += 1;

    if (action->parameters->size() > 0) {
        for (auto param : *action->parameters) {
            metrics.parametersNum[actionName] += 1;
            metrics.parameterSizeSum[actionName] += paramSize(param);
        }
        metrics.numActionsWithParameter += 1;
    }

    metrics.totalParameters += metrics.parametersNum[actionName];
    metrics.totalParameterSizeSum += metrics.parameterSizeSum[actionName];
    metrics.maxParametersPerAction =
        std::max(metrics.maxParametersPerAction, metrics.parametersNum[actionName]);
}

void ActionParameterMetricsPass::postorder(const IR::P4Program * /*program*/) {
    if (metrics.numActions > 0) {
        metrics.avgParametersPerAction =
            static_cast<double>(metrics.totalParameters) / static_cast<double>(metrics.numActions);
        metrics.avgParametersPerActionWithParameter = static_cast<double>(metrics.totalParameters) /
            static_cast<double>(metrics.numActionsWithParameter);
    }
    if (metrics.totalParameters > 0)
        metrics.avgParameterSize =
            static_cast<double>(metrics.totalParameterSizeSum) / static_cast<double>(metrics.totalParameters);
}
}  // namespace P4
