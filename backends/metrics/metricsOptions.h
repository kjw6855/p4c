#ifndef BACKENDS_METRICS_METRICSOPTIONS_H_
#define BACKENDS_METRICS_METRICSOPTIONS_H_

#include "frontends/common/options.h"
#include "backends/metrics/metricsStructure.h"
#include "lib/cstring.h"

using namespace P4::literals;

namespace P4::P4Metrics {

class MetricsOptions : public CompilerOptions {
 public:
    /// file to output to
    std::filesystem::path outputDir{"."};
    /// read from json
    bool loadIRFromJson = false;
    std::set<cstring> customSelectedMetrics;
    Metrics customMetrics;

    MetricsOptions();
};

using MetricsContext = P4CContextWithOptions<MetricsOptions>;

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_METRICSOPTIONS_H_ */