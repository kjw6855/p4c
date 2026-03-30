/*
Collects parser metrics by applying the CC calculator to each state,
and collecting the number of states of each encountered parser
*/

#ifndef BACKENDS_METRICS_PARSERMETRICS_H_
#define BACKENDS_METRICS_PARSERMETRICS_H_

#include "backends/metrics/cyclomaticComplexity.h"
#include "backends/metrics/metricsStructure.h"
#include "ir/ir.h"

namespace P4::P4Metrics {

class ParserMetricsPass : public Inspector {
 private:
    ParserMetrics &metrics;

 public:
    explicit ParserMetricsPass(Metrics &metricsRef) : metrics(metricsRef.parserMetrics) {
        setName("ParserMetricsPass");
    }

    bool preorder(const IR::P4Parser *parser) override;
};

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_PARSERMETRICS_H_ */
