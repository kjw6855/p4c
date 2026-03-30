#include "backends/metrics/externalObjectsMetric.h"

namespace P4::P4Metrics {

bool ExternalObjectsMetricPass::preorder(const IR::Type_Extern *node) {
    externTypeNames.insert(node->name.name);
    metrics.externStructures++;
    LOG2("Found extern structure: " << node->name.name);

    std::stringstream sstream;
    for (const auto &method : node->methods) {
        externMethods[node->name.name].insert(method->name.name);
        sstream << method->name.name << " ";
    }
    LOG2("Extern structure " << node->name.name << " has methods: " << sstream.str());
    return true;
}

bool ExternalObjectsMetricPass::preorder(const IR::Declaration_Instance *node) {
    const IR::Type *type = node->type;
    cstring typeName;

    if (auto tn = type->to<IR::Type_Name>()) {
        typeName = tn->path->name.name;
        LOG2("Found instance of type: " << typeName);
        if (externTypeNames.count(typeName)) {
            metrics.externStructUses++;
            metrics.externUsesPerStruct[typeName]++;
        }
    }
    // TODO: support Type_Specialized for Register
    return true;
}

bool ExternalObjectsMetricPass::preorder(const IR::Member *node) {
    auto baseType = node->expr->type;

    std::stringstream sstream;
    node->dbprint(sstream);
    auto nodeName = cstring(sstream);
    if (!nodeName.startsWith("hdr")) {
        LOG3("[MEMBER]: " << node);
    }

    if (baseType && baseType->is<IR::Type_Extern>()) {
        auto externType = baseType->to<IR::Type_Extern>();
        cstring externName = externType->name.name;
        cstring memberName = node->member.name;

        LOG2("Found member access: " << externName << "." << memberName);
        metrics.externStructUses++;
        metrics.externUsesPerStruct[externName]++;
        // Check if member is a method call and count it.
        if (externMethods.count(externName) && externMethods[externName].count(memberName)) {
            metrics.externFunctionUses++;
            auto key = externName + "."_cs + memberName;
            metrics.externUsesPerFunction[key]++;
        }
    }
    return true;
}

bool ExternalObjectsMetricPass::preorder(const IR::Method *node) {
    // Do not add methods that belong to an extern structure.
    if (!findContext<IR::Type_Extern>()) {
        metrics.externFunctions++;
        externFunctions.insert(node->name.name);
    }
    return true;
}

bool ExternalObjectsMetricPass::preorder(const IR::MethodCallExpression *node) {
    auto method = node->method;
    LOG3("[MCE]: " << node);

    if (auto path = method->to<IR::PathExpression>()) {
        cstring calledName = path->path->name.name;
        LOG2("Found method call: " << calledName);
        if (externFunctions.count(calledName)) {
            metrics.externFunctionUses++;
            metrics.externUsesPerFunction[calledName]++;
        }
    } else {
        auto *instance = P4::MethodInstance::resolve(node, refMap, typeMap);
        if (instance->is<P4::ExternMethod>()) {
            auto em = instance->to<P4::ExternMethod>();
            auto externName = em->originalExternType->getName().name;
            auto methodName = em->method->name.name;
            LOG2("Found extern method call: " << externName << "." << methodName);

            metrics.externFunctionUses++;
            auto key = externName + "."_cs + methodName;
            metrics.externUsesPerFunction[key]++;
            return false;
        }
    }
    return true;
}

/*
bool ExternalObjectsMetricPass::preorder(const IR::MethodCallStatement *statement) {
    auto instance = P4::MethodInstance::resolve(statement->methodCall, refMap, typeMap);
    return true;
}*/

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

}  // namespace P4::P4Metrics
