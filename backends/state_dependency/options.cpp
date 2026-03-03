#include "backends/state_dependency/options.h"

#include <utility>

namespace P4::P4StateDependency {

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
                auto selectionString = cstring(arg).toUpper();
                auto it = SHOW_VAR_OPTIONS.find(selectionString);
                if (it != SHOW_VAR_OPTIONS.end()) {
                    varVis = it->second;
                    return true;
                }
                std::set<cstring> printSet;
                std::transform(SHOW_VAR_OPTIONS.cbegin(), SHOW_VAR_OPTIONS.cend(),
                               std::inserter(printSet, printSet.begin()),
                               [](const std::pair<cstring, VarVisibility> &mapTuple) {
                                   return mapTuple.first;
                               });
                std::stringstream sstream;
                sstream << "[";
                for (auto it = printSet.begin(); it != printSet.end(); ++it) {
                    if (it != printSet.begin()) sstream << ", ";
                    sstream << *it;
                }
                sstream << "]";
                error(
                    "Variable visibility %1% not supported. Supported visibilities are "
                    "%2%.",
                    selectionString, cstring(sstream));
                return false;
            },
            "Use to show nodes' variables in graph.");
    registerOption(
            "--actionAsProc", nullptr,
            [this](const char *) {
            setActionAsProc = true;
            return true;
            },
            "Use to consider P4Action as procedure call.");
    registerOption(
            "--supergraph", nullptr,
            [this](const char *) {
            genSupergraphs = true;
            return true;
            },
            "Use if you want to create supergraph for IFDS.");
}

}  // namespace P4::P4StateDependency
