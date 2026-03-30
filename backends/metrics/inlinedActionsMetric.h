/*
Counts the number of actions inlined during the frontend stage by
counting unique action names inside of blocks with annotantions of type
"Annotation::inlinedFromAnnotation".
*/

#ifndef BACKENDS_METRICS_INLINEDACTIONSMETRIC_H_
#define BACKENDS_METRICS_INLINEDACTIONSMETRIC_H_

#include "backends/metrics/metricsStructure.h"
#include "ir/ir.h"

namespace P4::P4Metrics {

class InlinedActionsMetricPass : public Inspector {
 private:
    Metrics &metrics;
    std::unordered_set<cstring> actions;

 public:
    explicit InlinedActionsMetricPass(Metrics &metricsRef) : metrics(metricsRef) {
        setName("InlinedActionsMetricPass");
    }

    void postorder(const IR::BlockStatement *block) override;
};

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_INLINEDACTIONSMETRIC_H_ */
