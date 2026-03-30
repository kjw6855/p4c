#ifndef BACKENDS_METRICS_ACTIONPARAMETERMETRICS_H_
#define BACKENDS_METRICS_ACTIONPARAMETERMETRICS_H_

#include "backends/metrics/metricsStructure.h"
#include "frontends/p4/typeMap.h"
#include "ir/ir.h"

namespace P4::P4Metrics {

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

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_ACTIONPARAMETERMETRICS_H_ */
