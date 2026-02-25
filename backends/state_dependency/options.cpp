#include "backends/state_dependency/options.h"

#include "backends/p4tools/common/compiler/context.h"

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
            "--showVar", nullptr,
            [this](const char *) {
            showVar = true;
            return true;
            },
            "Use to show nodes' variables in graph.");
    registerOption(
            "--actionAsProc", nullptr,
            [this](const char *) {
            setActionAsProc = true;
            return true;
            },
            "Use to consider P4Action as procedure call.");
}

}  // namespace P4::P4StateDependency
