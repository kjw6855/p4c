#include "frontends/p4/metrics/externalObjectsMetric.h"

namespace P4 {

using namespace P4::literals;

void ExternalObjectsMetricPass::postorder(const IR::Type_Extern *node) {
    externTypeNames.insert(node->name.name);
    metrics.externStructures++;
    LOG2("Found extern structure: " << node->name.name);

    std::stringstream sstream;
    for (const auto &method : node->methods) {
        externMethods[node->name.name].insert(method->name.name);
        sstream << method->name.name << " ";
    }
    LOG2("Extern structure " << node->name.name << " has methods: " << sstream.str());
}

void ExternalObjectsMetricPass::postorder(const IR::Declaration_Instance *node) {
    const IR::Type *type = node->type;
    cstring typeName;

    if (auto tn = type->to<IR::Type_Name>()) {
        typeName = tn->path->name.name;
        if (externTypeNames.count(typeName)) {
            metrics.externStructUses++;
            metrics.externUsesPerStruct[typeName]++;
        }
    }
}

void ExternalObjectsMetricPass::postorder(const IR::Member *node) {
    auto baseType = node->expr->type;

    if (baseType && baseType->is<IR::Type_Extern>()) {
        auto externType = baseType->to<IR::Type_Extern>();
        cstring externName = externType->name.name;
        cstring memberName = node->member.name;

        metrics.externStructUses++;
        metrics.externUsesPerStruct[externName]++;
        // Check if member is a method call and count it.
        if (externMethods.count(externName) && externMethods[externName].count(memberName)) {
            metrics.externFunctionUses++;
            auto key = externName + "."_cs + memberName;
            metrics.externUsesPerFunction[key]++;
        }
    }
}

void ExternalObjectsMetricPass::postorder(const IR::Method *node) {
    // Do not add methods that belong to an extern structure.
    if (!findContext<IR::Type_Extern>()) {
        metrics.externFunctions++;
        externFunctions.insert(node->name.name);
    }
}

void ExternalObjectsMetricPass::postorder(const IR::MethodCallExpression *node) {
    auto method = node->method;

    if (auto path = method->to<IR::PathExpression>()) {
        cstring calledName = path->path->name.name;
        if (externFunctions.count(calledName)) {
            metrics.externFunctionUses++;
            metrics.externUsesPerFunction[calledName]++;
        }
    }
}

void ExternalObjectsMetricPass::postorder(const IR::P4Program * /*node*/) {
    if (!LOGGING(3)) return;

    std::cout << "Extern Functions (" << externFunctions.size() << "):\n";
    for (const auto &fn : externFunctions) {
        std::cout << " - " << fn << std::endl;
    }

    std::cout << "\nExtern Types and Methods per Type:\n";
    for (const auto &[typeName, methods] : externMethods) {
        std::cout << "  " << typeName << " (" << methods.size() << "):\n";
        for (const auto &method : methods) {
            std::cout << "   - " << method << std::endl;
        }
    }
}

}  // namespace P4
