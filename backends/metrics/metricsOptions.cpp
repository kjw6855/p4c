#include "metricsOptions.h"

namespace P4::P4Metrics {

MetricsOptions::MetricsOptions() {
    langVersion = CompilerOptions::FrontendVersion::P4_16;
    registerOption(
        "--out-dir", "outdir",
        [this](const char *arg) {
            outputDir = arg;
            return true;
        },
        "Write output to outfile");
    registerOption(
        "--fromJSON", "file",
        [this](const char *arg) {
            loadIRFromJson = true;
            file = arg;
            return true;
        },
        "Use IR representation from JsonFile dumped previously");
    registerOption(
        "--custom-metrics", "metric1[,metric2]",
        [this](const char *arg) {
            static const std::set<cstring> validMetrics = {"loc"_cs,
                                                           "action-param"_cs,
                                                           "cyclomatic"_cs,
                                                           "halstead"_cs,
                                                           "unused-code"_cs,
                                                           "nesting-depth"_cs,
                                                           "header-general"_cs,
                                                           "header-manipulation"_cs,
                                                           "header-modification"_cs,
                                                           "match-action"_cs,
                                                           "parser"_cs,
                                                           "inlined"_cs,
                                                           "extern"_cs};
            auto copy = strdup(arg);
            while (cstring metric = cstring(strsep(&copy, ","))) {
                if (metric == "all") {
                    customSelectedMetrics = validMetrics;
                    return true;
                } else if (validMetrics.find(metric) == validMetrics.end()) {
                    ::P4::error(ErrorType::ERR_INVALID, "Invalid metric: %s", metric);
                    return false;
                } else {
                    customSelectedMetrics.insert(metric);
                }
            }
            return true;
        },
        "Select which code metrics will be collected (custom).\n"
        "Valid options: all, loc, action-param, cyclomatic, halstead, unused-code,\n"
        "duplicit-code, nesting-depth, header-general, header-manipulation,\n"
        "header-modification, match-action, parser, inlined, extern.");
}

}  // namespace P4::P4Metrics