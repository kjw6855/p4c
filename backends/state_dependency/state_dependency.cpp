/**
 * @author Jiwon Kim
 */

#include "backends/state_dependency/version.h"
#include "backends/state_dependency/state_dependency.h"
#include "backends/state_dependency/options.h"

#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/parseInput.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/frontend.h"

#include "backends/state_dependency/graphs.h"
#include "ir/ir.h"
#include "ir/json_loader.h"
#include "ir/pass_utils.h"
#include "lib/crash.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/gc.h"
#include "lib/log.h"
#include "lib/nullstream.h"
#include "lib/timer.h"

#include "utils.h"
#include "graphs.h"
#include "controls.h"
#include "parsers.h"
#include "graph_visitor.h"
#include "dependency_graph.h"
#include "supergraphs.h"
#include "ide_pass.h"
#include "act_param_to_stateful.h"
#include "stateful_to_key.h"
#include "hdr_to_stateful.h"

namespace P4::P4StateDependency {

class MidEnd : public PassManager {
 public:
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    IR::ToplevelBlock *toplevel = nullptr;

    explicit MidEnd(CompilerOptions &options);
    IR::ToplevelBlock *process(const IR::P4Program *&program) {
        program = program->apply(*this);
        return toplevel;
    }
};

MidEnd::MidEnd(CompilerOptions &options) {
    bool isv1 = options.langVersion == CompilerOptions::FrontendVersion::P4_14;
    refMap.setIsV1(isv1);
    auto evaluator = new P4::EvaluatorPass(&refMap, &typeMap);
    setName("MidEnd");

    addPasses({
        new P4::TypeChecking(&refMap, &typeMap, true),  // update types before ComputeDefUse
        evaluator,
        [this, evaluator]() { toplevel = evaluator->getToplevelBlock(); },
    });
}

}  // namespace P4::P4StateDependency

using namespace P4;
using P4StateDependencyContext = P4CContextWithOptions<::P4StateDependency::P4StateDependencyOptions>;

using VarEdgeVisibility = P4StateDependency::VarEdgeVisibility;

int main(int argc, char *const argv[]) {
    setup_gc_logging();
    setup_signals();

    AutoCompileContext autoP4StateDependencyContext(new P4StateDependencyContext);
    auto &options = P4StateDependencyContext::get().options();

    options.langVersion = CompilerOptions::FrontendVersion::P4_16;
    options.compilerVersion = cstring(P4C_STATE_DEPENDENCY_VERSION_STRING);

    if (options.process(argc, argv) != nullptr) {
        if (options.loadIRFromJson == false) options.setInputFile();
    }
    if (::P4::errorCount() > 0) return 1;

    auto hook = options.getDebugHook();

    const IR::P4Program *program = nullptr;

    if (options.loadIRFromJson) {
        std::filebuf fb;
        if (fb.open(options.file, std::ios::in) == nullptr) {
            ::P4::error(ErrorType::ERR_IO, "%s: No such file or directory.", options.file);
            return 1;
        }

        std::istream inJson(&fb);
        JSONLoader jsonFileLoader(inJson);
        if (!jsonFileLoader) {
            ::P4::error(ErrorType::ERR_IO, "Not valid input file");
            return 1;
        }
        Util::ScopedTimer jsonTimer("json loading");
        program = new IR::P4Program(jsonFileLoader);
        fb.close();
    } else {
        Util::ScopedTimer frontendTimer("P4 compile frontend");
        program = P4::parseP4File(options);
        if (program == nullptr || ::P4::errorCount() > 0) return 1;

        try {
            P4::P4COptionPragmaParser optionsPragmaParser(false);
            program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

            P4::FrontEnd fe;
            fe.addDebugHook(hook);
            program = fe.run(options, program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }
        if (program == nullptr || ::P4::errorCount() > 0) return 1;
    }

    P4StateDependency::MidEnd midEnd(options);
    midEnd.addDebugHook(hook);
    const IR::ToplevelBlock *top = nullptr;
    {
        Util::ScopedTimer midendTimer("P4 compile midend");
        try {
            top = midEnd.process(program);
            if (!options.dumpJsonFile.empty())
                JSONGenerator(*openFile(options.dumpJsonFile, true)).emit(program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }
    }
    if (::P4::errorCount() > 0) return 1;

    BUG_CHECK(options.arch, "Architecture must be specified with --arch option");

    P4StateDependency::ControlGraphs cgen(&midEnd.refMap, &midEnd.typeMap,
            options.graphsDir, options.arch);
    // TODO: set options in contructor
    cgen.varVis = options.varVis;
    cgen.genSupergraphs = options.genSupergraphs;

    P4StateDependency::SuperGraphs *sg = nullptr;
    P4StateDependency::ActParamToStateful *sdChecker = nullptr;
    P4StateDependency::StatefulToKey *pdChecker = nullptr;
    P4StateDependency::HdrToStateful *hdChecker = nullptr;
    P4StateDependency::ParserGraphs *pgg = nullptr;
    // All SO_DATA
    hvec_map<cstring, std::vector<P4StateDependency::TabVertex>> stateVars;
    hvec_map<cstring, P4StateDependency::IDEPass::DepEdgeMap> depEdgeMaps;

    {
        Util::ScopedTimer sdTimer("P4SD");
        LOG2("Generating graphs under " << options.graphsDir);
        LOG2("Generating control graphs");
        {
            Util::ScopedTimer cfgTimer("CFG");
            top->getMain()->apply(cgen);
        }

        if (options.genSupergraphs != P4StateDependency::GenSGMode::NONE) {
            {
                Util::ScopedTimer esgTimer("ESG");
                sg = new P4StateDependency::SuperGraphs(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &cgen.graphVars,
                        &cgen.graphLocalVars,
                        &cgen.procOfs,
                        &cgen.callMaps,
                        &cgen.procCallerMaps,
                        &cgen.retArgEdges,
                        &cgen.actionMaps,
                        &cgen.headerVarNames,
                        &cgen.ingressPortVars,
                        &cgen.egressPortVars,
                        &cgen.dropVars);
                // generate supergraphs
                sg->gen_supergraphs();
            }

            /* I. A2S2V */
            P4StateDependency::DependencyGraphs a2s2vGraphs(cgen.controlGraphsArray.size());
            {
                // State dependency checker
                Util::ScopedTimer actToSoTimer("ACT->SO");
                sdChecker = new P4StateDependency::ActParamToStateful(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &sg->graphProps,
                        options.genSupergraphs);
                program->apply(*sdChecker);
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    const auto &ptsEdgeMap = sdChecker->getFoundDepEdges(graphName);
                    P4StateDependency::IDEPass::DepEdgeMap stateVarMap;
                    stateVars[graphName] = collect_state_vars_from_dep_edges_dst(cgen.controlGraphsArray[i],
                        sg->graphProps[i], &midEnd.refMap, &midEnd.typeMap,
                        ptsEdgeMap, stateVarMap, true);
                    a2s2vGraphs.add_dependencies_from_map(i, g, ptsEdgeMap);
                    a2s2vGraphs.add_dependencies_from_map(i, g, stateVarMap);
                    // Convert depEdges since ptsEdgeMap contains action -> SO
                    depEdgeMaps[graphName] = P4StateDependency::convert_dep_edges(ptsEdgeMap);
                    for (const auto &ve : ptsEdgeMap) {
                        for (const auto &dst : ve.second) {
                            LOG2(P4StateDependency::Graphs::dump_var_edge(g, {ve.first, dst}));
                        }
                    }
                }
            }

            /* TODO: while loop for SO->SO */

            {
                // Packet dependency checker
                Util::ScopedTimer actSoToKeyTimer("ACT->SO->KEY/HDR");
                pdChecker = new P4StateDependency::StatefulToKey(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &sg->graphProps,
                        options.genSupergraphs,
                        &stateVars,
                        &depEdgeMaps,
                        "A2S2V"_cs);
                program->apply(*pdChecker);
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    a2s2vGraphs.add_dependencies_from_map(i, g,
                        pdChecker->getFoundDepEdges(graphName), true);
                }
            }

            {
                Util::ScopedTimer actSoToKeyDrawTimer("ACT->SO->KEY/HDR drawing");
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    if (a2s2vGraphs.leaves[i].empty()) continue;  // Skip pruning if no leaves

                    a2s2vGraphs.prune_nodes_not_reaching_leaves(i);
                    if (a2s2vGraphs.num_vertices(i) == 0) continue;
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    a2s2vGraphs.export_to_graphviz(i, options.graphsDir / (graphName + "_a2s2v_dep.dot"));
                }
            }

            stateVars.clear();
            depEdgeMaps.clear();
            /* II. H2S2V */
            P4StateDependency::DependencyGraphs h2s2vGraphs(cgen.controlGraphsArray.size());
            {
                Util::ScopedTimer hdrToStatefulTimer("HDR->SO");
                hdChecker = new P4StateDependency::HdrToStateful(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &sg->graphProps,
                        options.genSupergraphs);
                program->apply(*hdChecker);

                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    // htsEdgeMap: stateful objects -> header variables
                    const auto &htsEdgeMap = hdChecker->getFoundDepEdges(graphName);
                    P4StateDependency::IDEPass::DepEdgeMap stateVarMap;
                    stateVars[graphName] = collect_state_vars_from_dep_edges_src(cgen.controlGraphsArray[i],
                        sg->graphProps[i], &midEnd.refMap, &midEnd.typeMap,
                        htsEdgeMap, stateVarMap, true);
                    // Do not convert depEdges since htsEdgeMap contains stateful objects -> header variables
                    depEdgeMaps[graphName] = htsEdgeMap;
                    h2s2vGraphs.add_dependencies_from_map(i, g, P4StateDependency::convert_dep_edges(htsEdgeMap));
                    h2s2vGraphs.add_dependencies_from_map(i, g, stateVarMap);
                    for (const auto &ve : htsEdgeMap) {
                        for (const auto &dst : ve.second) {
                            LOG2(P4StateDependency::Graphs::dump_var_edge(g, {ve.first, dst}));
                        }
                    }
                }
            }

            {
                // Packet dependency checker
                Util::ScopedTimer hdrSoToKeyTimer("HDR->SO->KEY/HDR");
                pdChecker = new P4StateDependency::StatefulToKey(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &sg->graphProps,
                        options.genSupergraphs,
                        &stateVars,
                        &depEdgeMaps,
                        "H2S2V"_cs);
                program->apply(*pdChecker);
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    h2s2vGraphs.add_dependencies_from_map(i, g,
                        pdChecker->getFoundDepEdges(graphName), true);
                }
            }

            {
                Util::ScopedTimer hdrSoToKeyDrawTimer("HDR->SO->KEY/HDR drawing");
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    if (h2s2vGraphs.leaves[i].empty()) continue;  // Skip pruning if no leaves

                    h2s2vGraphs.prune_nodes_not_reaching_leaves(i);
                    if (h2s2vGraphs.num_vertices(i) == 0) continue;
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    h2s2vGraphs.export_to_graphviz(i, options.graphsDir / (graphName + "_h2s2v_dep.dot"));
                }
            }

            stateVars.clear();
            depEdgeMaps.clear();
            /* III. SO->KEY/HDR */
            {
                Util::ScopedTimer soToKeyTimer("SO->KEY/HDR");
                for (size_t i = 0; i < cgen.controlGraphsArray.size(); i++) {
                    auto *g = cgen.controlGraphsArray[i];
                    auto graphName = boost::get_property(*g, boost::graph_name);
                    stateVars[graphName] = collect_state_vars(cgen.controlGraphsArray[i],
                        sg->graphProps[i], &midEnd.refMap, &midEnd.typeMap, true);
                }
                pdChecker = new P4StateDependency::StatefulToKey(&midEnd.refMap, &midEnd.typeMap,
                        &cgen.controlGraphsArray,
                        &sg->graphProps,
                        options.genSupergraphs,
                        &stateVars,
                        &depEdgeMaps,
                        "S2V"_cs);
                program->apply(*pdChecker);
            }
        }
    }

    /*
     * TODO: Show the full dependency:
     * e.g., Data Plane Dependency: Set of header types -> stateful object -> header/key
     * e.g., Control Plane Dependency: Action parameter -> stateful object -> header/key
     */
    {
        Util::ScopedTimer parserTimer("Parser graphs");
        LOG2("Generating parser graphs");
        pgg = new P4StateDependency::ParserGraphs(&midEnd.refMap, options.graphsDir);
        program->apply(*pgg);
    }

    {
        Util::ScopedTimer drawTimer("Drawing graphs");
        if (options.varEdgeVis == VarEdgeVisibility::ACTION_PARAM ||
                options.varEdgeVis == VarEdgeVisibility::ALL) {
            sdChecker->set_edge_func();
        }
        if (options.varEdgeVis == VarEdgeVisibility::STATEFUL_OBJECT ||
                options.varEdgeVis == VarEdgeVisibility::ALL) {
            pdChecker->set_edge_func();
        }
        if (options.varEdgeVis == VarEdgeVisibility::HDR_TO_STATEFUL ||
                options.varEdgeVis == VarEdgeVisibility::ALL) {
            hdChecker->set_edge_func();
        }
        P4StateDependency::GraphVisitor gvs(options.graphsDir, options.graphs,
                options.fullGraph, options.jsonOut, options.file,
                options.varVis, options.varEdgeVis);

        gvs.process(cgen.controlGraphsArray, pgg->parserGraphsArray);
    }

    P4StateDependency::printPerformanceReport();

    delete pgg;
    delete pdChecker;
    delete sdChecker;
    delete hdChecker;
    delete sg;
    return ::P4::errorCount() > 0;
}
