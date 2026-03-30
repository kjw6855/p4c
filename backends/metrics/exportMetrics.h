/*
Exports the collected code metric values into a formatted
text file, and a json file. The new filenames are based on the
compiled program name (programName_metrics.txt/json).
*/

#ifndef BACKENDS_METRICS_EXPORTMETRICS_H_
#define BACKENDS_METRICS_EXPORTMETRICS_H_

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

#include "backends/metrics/metricsStructure.h"
#include "ir/ir.h"
#include "lib/json.h"

using namespace P4::literals;

namespace P4::P4Metrics {

class ExportMetricsPass : public Inspector {
 private:
    std::filesystem::path filename;
    std::filesystem::path dirname;
    std::set<cstring> selectedMetrics;
    Metrics &metrics;

 public:
    explicit ExportMetricsPass(const std::filesystem::path &filename,
                               const std::filesystem::path &dirname,
                               std::set<cstring> selectedMetrics, Metrics &metricsRef)
        : filename(filename), dirname(dirname), selectedMetrics(selectedMetrics), metrics(metricsRef) {
        setName("ExportMetricsPass");
    }
    bool preorder(const IR::P4Program * /*program*/) override;
};

}  // namespace P4::P4Metrics

#endif /* BACKENDS_METRICS_EXPORTMETRICS_H_ */
