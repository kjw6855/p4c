/*
Adds code metric collection passes to the frontend pipeline,
based on the "selectedMetrics" option. If any metrics were
selected by the user, the pass which exports them is added
as well.
*/

#ifndef BACKENDS_METRICS_METRICSPASSMANAGER_H_
#define BACKENDS_METRICS_METRICSPASSMANAGER_H_

#include <filesystem>

#include "frontends/p4/frontend.h"
#include "frontends/common/options.h"
#include "backends/metrics/actionParameterMetrics.h"
#include "backends/metrics/cyclomaticComplexity.h"
#include "backends/metrics/exportMetrics.h"
#include "backends/metrics/externalObjectsMetric.h"
#include "backends/metrics/halsteadMetrics.h"
#include "backends/metrics/headerMetrics.h"
#include "backends/metrics/headerPacketMetrics.h"
#include "backends/metrics/inlinedActionsMetric.h"
#include "backends/metrics/linesOfCodeMetric.h"
#include "backends/metrics/matchActionTableMetrics.h"
#include "backends/metrics/metricsStructure.h"
#include "backends/metrics/nestingDepthMetric.h"
#include "backends/metrics/parserMetrics.h"
#include "backends/metrics/unusedCodeMetric.h"
#include "backends/metrics/metricsOptions.h"
#include "ir/ir.h"

using namespace P4::literals;

namespace P4::P4Metrics {

class MetricsPassManager {
 private:
    const std::set<cstring> &selectedMetrics;
    ReferenceMap *refMap;
    TypeMap *typeMap;
    Metrics &metrics;
    std::filesystem::path fileName;
    std::filesystem::path dirName;

 public:
    MetricsPassManager(const MetricsOptions &options, ReferenceMap *refMap, TypeMap *typeMap, Metrics &metricsRef)
        : selectedMetrics(options.customSelectedMetrics),
          refMap(refMap),
          typeMap(typeMap),
          metrics(metricsRef),
          fileName(options.file),
          dirName(options.outputDir) {}

    Metrics &getMetrics() { return metrics; }
    void addInlined(PassManager &pm);
    void addUnusedCode(PassManager &pm, bool isBefore);
    void addMetricPasses(PassManager &pm);
};

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_METRICSPASSMANAGER_H_ */
