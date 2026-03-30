/*
Counts the number of external structure and function declarations
and uses. When encountering an extern structure declaration, its
method names are collected into "externFunctions". Encountering
an extern method is counted as an extern structure use and as an extern
function call too.
*/

#ifndef BACKENDS_METRICS_EXTERNALOBJECTSMETRIC_H_
#define BACKENDS_METRICS_EXTERNALOBJECTSMETRIC_H_

#include <set>

#include "frontends/p4/frontend.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "backends/metrics/metricsStructure.h"
#include "ir/ir.h"

namespace P4::P4Metrics {

class ExternalObjectsMetricPass : public Inspector {
 private:
    ReferenceMap *refMap;
    TypeMap *typeMap;
    ExternMetrics &metrics;
    std::set<cstring> externFunctions;                   // Standalone extern functions.
    std::set<cstring> externTypeNames;                   // Type names of extern structures.
    std::map<cstring, std::set<cstring>> externMethods;  // Tracks methods per extern.

 public:
    explicit ExternalObjectsMetricPass(ReferenceMap *refMap, TypeMap *typeMap, Metrics &metricsRef)
    : refMap(refMap), typeMap(typeMap), metrics(metricsRef.externMetrics) {
        setName("ExternalObjectsMetricPass");
    }

    /// Extern structure declaration.
    bool preorder(const IR::Type_Extern *node) override;
    /// Extern structure instance.
    bool preorder(const IR::Declaration_Instance *node) override;
    /// Extern structure use.
    bool preorder(const IR::Member *node) override;
    /// Extern function declaration.
    bool preorder(const IR::Method *node) override;
    /// Extern function call.
    bool preorder(const IR::MethodCallExpression *node) override;
    /// Print contents of helper sets to stdout if logging is enabled.
    void postorder(const IR::P4Program * /*node*/) override;
};

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_EXTERNALOBJECTSMETRIC_H_ */
