#include "backends/state_dependency/options.h"

#include <utility>

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
            "--actionAsProc", nullptr,
            [this](const char *) {
            setActionAsProc = true;
            return true;
            },
            "Use to consider P4Action as procedure call.");
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
            "--showVarEdge", nullptr,
            [this](const char *) {
            showVarEdgeLabel = true;
            return true;
            },
            "Use to show variable edge label in graph.");
}

}  // namespace P4::P4StateDependency
