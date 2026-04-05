#include "backends/state_dependency/options.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <boost/format.hpp>

#include "lib/log.h"
#include "lib/timer.h"


namespace P4::P4StateDependency {

template <typename Enum>
bool pares_enum_option(const char* arg,
                     const std::map<cstring, Enum>& options,
                     Enum& out,
                     const char* what) {
    auto selectionString = cstring(arg).toUpper();
    auto it = options.find(selectionString);
    if (it != options.end()) {
        out = it->second;
        return true;
    }

    std::set<cstring> printSet;
    std::transform(options.cbegin(), options.cend(),
            std::inserter(printSet, printSet.begin()),
            [](const std::pair<cstring, Enum> &mapTuple) {
                return mapTuple.first;
            });

    std::stringstream sstream;
    sstream << "[";
    for (auto it2 = printSet.begin(); it2 != printSet.end(); ++it2) {
        if (it2 != printSet.begin()) sstream << ", ";
        sstream << *it2;
    }
    sstream << "]";

    error("%1% %2% not supported. Supported values are %3%.",
            what, selectionString, cstring(sstream));
    return false;
}

void enablePerformanceLogging() { Log::addDebugSpec("tools_performance:4"); }

/* Copied from backends/p4tools/modules/common/lib/logging.h */
inline std::string logHelper(boost::format &f) { return f.str(); }

/// Helper function for @printFeature
template <class T, class... Args>
std::string logHelper(boost::format &f, T &&t, Args &&...args) {
    return logHelper(f % std::forward<T>(t), std::forward<Args>(args)...);
}

template <typename... Arguments>
void printFeature(const std::string &label, int level, const std::string &fmt,
                  Arguments &&...args) {
    // Do not print logging messages when logging is not enabled.
    if (!Log::fileLogLevelIsAtLeast(label.c_str(), level)) {
        return;
    }

    boost::format f(fmt);
    LOG_FEATURE(label.c_str(), level, logHelper(f, std::forward<Arguments>(args)...));
}

void printPerformanceReport(const std::optional<std::filesystem::path> &basePath) {
    // Do not emit a report if performance logging is not enabled.
    if (!Log::fileLogLevelIsAtLeast("tools_performance", 4)) {
        return;
    }
    printFeature("tools_performance", 4, "============ Timers ============");
    using TimerData = std::unordered_map<std::string, std::string>;
    std::vector<TimerData> timerList;
    for (const auto &c : Util::getTimers()) {
        TimerData timerData;
        timerData["time"] = std::to_string(c.milliseconds);
        if (c.timerName.empty()) {
            printFeature("tools_performance", 4, "Total: %i ms", c.milliseconds);
            timerData["pct"] = "100";
            timerData["name"] = "total";
            timerData["invocations"] = std::to_string(c.invocations);
        } else {
            timerData["pct"] = std::to_string(c.relativeToParent * 100);
            auto timePerInvocation =
                static_cast<float>(c.milliseconds) / static_cast<float>(c.invocations);
            printFeature("tools_performance", 4,
                         "%s: %i ms (%i ms per invocation, %0.2f %% of parent)", c.timerName,
                         c.milliseconds, timePerInvocation, c.relativeToParent * 100);
            auto prunedName = c.timerName;
            prunedName.erase(remove_if(prunedName.begin(), prunedName.end(), isspace),
                             prunedName.end());
            timerData["name"] = prunedName;
            timerData["invocations"] = std::to_string(c.invocations);
        }
        timerList.emplace_back(timerData);
    }
    // Write the report to the file, if one was provided.
    if (basePath.has_value()) {
        auto perfFilePath = basePath.value();
        perfFilePath.concat("_perf");
        perfFilePath.replace_extension(".csv");
        auto perfFile = std::ofstream(perfFilePath, std::ios::out | std::ios::app);
        if (!perfFile.is_open()) {
            error("Failed to open the performance report file %1%", perfFilePath.c_str());
            return;
        }

        perfFile << "Timer,Total Time,Percentage\n";
        for (const auto &timerData : timerList) {
            perfFile << timerData.at("name") << "," << timerData.at("time") << ","
                     << timerData.at("pct") << "," << timerData.at("invocations") << "\n";
        }
        perfFile.close();
    }
}

P4StateDependencyOptions::P4StateDependencyOptions() {
    registerOption(
            "--graphs-dir", "dir",
            [this](const char *arg) {
            graphsDir = arg;
            return true;
            },
            "Use this directory to dump graphs in dot format "
            "(default is current working directory)\n");
    registerOption(
            "--fromJSON", "file",
            [this](const char *arg) {
            loadIRFromJson = true;
            file = arg;
            return true;
            },
            "Use IR representation from JsonFile dumped previously, "
            "the compilation starts with reduced midEnd.");
    registerOption(
            "--graphs", nullptr,
            [this](const char *) {
            graphs = true;
            isGraphsSet = true;
            return true;
            },
            "Use if you want default behavior - generation of separate graphs "
            "for each program block (enabled by default, "
            "if options --fullGraph or --jsonOut are not present).");
    registerOption(
            "--fullGraph", nullptr,
            [this](const char *) {
            fullGraph = true;
            if (!isGraphsSet) graphs = false;
            return true;
            },
            "Use if you want to generate graph depicting control flow "
            "through all program blocks (fullGraph).");
    registerOption(
            "--jsonOut", nullptr,
            [this](const char *) {
            jsonOut = true;
            if (!isGraphsSet) graphs = false;
            return true;
            },
            "Use to generate json output of fullGraph.");
    registerOption(
            "--showVar", "varVis",
            [this](const char *arg) {

                static std::map<cstring, VarVisibility> const SHOW_VAR_OPTIONS = {
                    {"NONE"_cs, VarVisibility::NONE},
                    {"REACHABLE"_cs, VarVisibility::REACHABLE},
                    {"FULL"_cs, VarVisibility::FULL},
                };

                return pares_enum_option(arg, SHOW_VAR_OPTIONS,
                        varVis, "Variable visibility");

            },
            "Use to show nodes' variables in graph.");
    registerOption(
            "--supergraph", "GenSGMode",
            [this](const char *arg) {
                static std::map<cstring, GenSGMode> const SUPERGRAPH_OPTIONS = {
                    {"NONE"_cs, GenSGMode::NONE},
                    {"ON_DEMAND"_cs, GenSGMode::ON_DEMAND},
                    {"FULL"_cs, GenSGMode::FULL},
                };

                return pares_enum_option(arg, SUPERGRAPH_OPTIONS,
                        genSupergraphs, "Generating supergraphs");

            },
            "Use if you want to create supergraph for IFDS.");
    registerOption(
            "--showVarEdge", "varEdgeVis",
            [this](const char *arg) {
                static std::map<cstring, VarEdgeVisibility> const SHOW_VAR_EDGE_OPTIONS = {
                    {"NONE"_cs, VarEdgeVisibility::NONE},
                    {"ACTION_PARAM"_cs, VarEdgeVisibility::ACTION_PARAM},
                    {"STATEFUL_OBJECT"_cs, VarEdgeVisibility::STATEFUL_OBJECT},
                    {"ALL"_cs, VarEdgeVisibility::ALL},
                };

                return pares_enum_option(arg, SHOW_VAR_EDGE_OPTIONS,
                        varEdgeVis, "Variable visibility");

            },
            "Use to show variable edge label in graph.");
    registerOption(
        "--print-performance-report", nullptr,
        [](const char *) {
            enablePerformanceLogging();
            return true;
        },
        "Print timing report summary at the end of the program.");
}

}  // namespace P4::P4StateDependency
