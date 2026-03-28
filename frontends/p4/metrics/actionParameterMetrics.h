#ifndef FRONTENDS_P4_METRICS_ACTIONPARAMETERMETRICS_H_
#define FRONTENDS_P4_METRICS_ACTIONPARAMETERMETRICS_H_

#include "frontends/p4/metrics/metricsStructure.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "ir/ir.h"

namespace P4 {

class ActionParameterMetricsPass : public Inspector {
 private:
    unsigned paramSize(const IR::Parameter *param);
    TypeMap *typeMap;
    ActionParameterMetrics &metrics;

 public:
    explicit ActionParameterMetricsPass(TypeMap *map, Metrics &metricsRef)
        : typeMap(map), metrics(metricsRef.actionParameterMetrics) {
        setName("ActionParameterMetricsPass");
    }

    void postorder(const IR::P4Action *action) override;
    void postorder(const IR::P4Program * /*program*/) override;
};

}  // namespace P4

#endif /* FRONTENDS_P4_METRICS_ACTIONPARAMETERMETRICS_H_ */
