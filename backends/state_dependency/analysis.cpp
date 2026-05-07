#include "backends/state_dependency/analysis.h"

#include <boost/graph/graph_traits.hpp>

#include "backends/state_dependency/act_param_to_stateful.h"
#include "backends/state_dependency/controls.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/state_dependency/hdr_to_stateful.h"
#include "backends/state_dependency/ide_pass.h"
#include "backends/state_dependency/stateful_to_key.h"
#include "backends/state_dependency/supergraphs.h"
#include "backends/state_dependency/utils.h"
#include "lib/hvec_map.h"
#include "lib/log.h"
#include "lib/timer.h"

namespace P4::P4StateDependency {

using namespace P4::literals;

StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  P4::ReferenceMap *refMap,
                                                  P4::TypeMap *typeMap,
                                                  const IR::ToplevelBlock *toplevel,
                                                  cstring arch,
                                                  std::filesystem::path graphsDir) {
    Util::ScopedTimer sdTimer("P4SD");
    StateDependencyResult result;

    // Heap-allocate ControlGraphs so it can be returned to the caller in binary mode
    // (needed by GraphVisitor). Library-mode callers should delete it themselves or
    // simply ignore result.cfgGraphs.
    auto *cgenRaw = new ControlGraphs(refMap, typeMap, graphsDir, arch);
    ControlGraphs &cgen = *cgenRaw;
    cgen.genSupergraphs = GenSGMode::FULL;
    toplevel->getMain()->apply(cgen);

    // Build IFDS supergraphs over the CFGs.
    SuperGraphs sg(refMap, typeMap,
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
    sg.gen_supergraphs();

    const size_t numGraphs = cgen.controlGraphsArray.size();
    hvec_map<cstring, std::vector<TabVertex>> stateVars;
    hvec_map<cstring, IDEPass::DepEdgeMap> depEdgeMaps;

    /* I. A2S2V: action parameter → stateful object → packet field */
    auto *a2s2vGraphs = new DependencyGraphs(numGraphs);
    auto *sdChecker = new ActParamToStateful(refMap, typeMap,
            &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL);
    {
        Util::ScopedTimer actToSoTimer("ACT->SO");
        program->apply(*sdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            const auto &ptsEdgeMap = sdChecker->getFoundDepEdges(graphName);
            IDEPass::DepEdgeMap stateVarMap;
            stateVars[graphName] = collect_state_vars_from_dep_edges_dst(
                    g, sg.graphProps[i], refMap, typeMap, ptsEdgeMap, stateVarMap, true);
            a2s2vGraphs->add_dependencies_from_map(i, g, ptsEdgeMap);
            a2s2vGraphs->add_dependencies_from_map(i, g, stateVarMap);
            depEdgeMaps[graphName] = convert_dep_edges(ptsEdgeMap);
            for (const auto &ve : ptsEdgeMap)
                for (const auto &dst : ve.second)
                    LOG2(Graphs::dump_var_edge(g, {ve.first, dst}));
        }
    }
    {
        Util::ScopedTimer actSoToKeyTimer("ACT->SO->KEY/HDR");
        StatefulToKey a2s2vPdChecker(refMap, typeMap,
                &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                &stateVars, &depEdgeMaps, "A2S2V"_cs);
        program->apply(a2s2vPdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            a2s2vGraphs->add_dependencies_from_map(i, g,
                    a2s2vPdChecker.getFoundDepEdges(graphName), true);
        }
    }
    {
        Util::ScopedTimer actSoToKeyDrawTimer("ACT->SO->KEY/HDR drawing");
        for (size_t i = 0; i < numGraphs; i++) {
            if (a2s2vGraphs->leaves[i].empty()) continue;
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_a2s2v_dep.dot"));
            a2s2vGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_a2s2v_dep.dot"));
            a2s2vGraphs->prune_nodes_not_reaching_leaves(i);
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_a2s2v_dep.dot"));
        }
    }
    result.a2s2vGraphs = a2s2vGraphs;

    stateVars.clear();
    depEdgeMaps.clear();

    /* II. H2S2V: header variable → stateful object → packet field */
    auto *h2s2vGraphs = new DependencyGraphs(numGraphs);
    auto *hdChecker = new HdrToStateful(refMap, typeMap,
            &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL);
    {
        Util::ScopedTimer hdrToStatefulTimer("HDR->SO");
        program->apply(*hdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            const auto &htsEdgeMap = hdChecker->getFoundDepEdges(graphName);
            IDEPass::DepEdgeMap stateVarMap;
            stateVars[graphName] = collect_state_vars_from_dep_edges_src(
                    g, sg.graphProps[i], refMap, typeMap, htsEdgeMap, stateVarMap, true);
            depEdgeMaps[graphName] = htsEdgeMap;
            h2s2vGraphs->add_dependencies_from_map(i, g, convert_dep_edges(htsEdgeMap));
            h2s2vGraphs->add_dependencies_from_map(i, g, stateVarMap);
            for (const auto &ve : htsEdgeMap)
                for (const auto &dst : ve.second)
                    LOG2(Graphs::dump_var_edge(g, {ve.first, dst}));
        }
    }
    {
        Util::ScopedTimer hdrSoToKeyTimer("HDR->SO->KEY/HDR");
        StatefulToKey h2s2vPdChecker(refMap, typeMap,
                &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                &stateVars, &depEdgeMaps, "H2S2V"_cs);
        program->apply(h2s2vPdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            h2s2vGraphs->add_dependencies_from_map(i, g,
                    h2s2vPdChecker.getFoundDepEdges(graphName), true);
        }
    }
    {
        Util::ScopedTimer hdrSoToKeyDrawTimer("HDR->SO->KEY/HDR drawing");
        for (size_t i = 0; i < numGraphs; i++) {
            if (h2s2vGraphs->leaves[i].empty()) continue;
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            if (!graphsDir.empty() && h2s2vGraphs->num_vertices(i) > 0)
                h2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_h2s2v_dep.dot"));
            h2s2vGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && h2s2vGraphs->num_vertices(i) > 0)
                h2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_h2s2v_dep.dot"));
            h2s2vGraphs->prune_nodes_not_reaching_leaves(i);
            if (!graphsDir.empty() && h2s2vGraphs->num_vertices(i) > 0)
                h2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_h2s2v_dep.dot"));
        }
    }
    result.h2s2vGraphs = h2s2vGraphs;

    /* III. S2V: stateful object → key/header (binary-only; only runs when graphsDir is set) */
    StatefulToKey *s2vChecker = nullptr;
    if (!graphsDir.empty()) {
        stateVars.clear();
        depEdgeMaps.clear();
        {
            Util::ScopedTimer soToKeyTimer("SO->KEY/HDR");
            for (size_t i = 0; i < numGraphs; i++) {
                auto *g = cgen.controlGraphsArray[i];
                auto graphName = cstring(boost::get_property(*g, boost::graph_name));
                stateVars[graphName] = collect_state_vars(g, sg.graphProps[i], refMap, typeMap, true);
            }
            s2vChecker = new StatefulToKey(refMap, typeMap,
                    &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                    &stateVars, &depEdgeMaps, "S2V"_cs);
            program->apply(*s2vChecker);
        }
    }

    // In binary mode, return objects the caller needs for CFG visualization.
    // In library mode, clean up and leave these null.
    if (!graphsDir.empty()) {
        result.cfgGraphs = cgenRaw;
        result.sdChecker = sdChecker;
        result.hdChecker = hdChecker;
        result.s2vChecker = s2vChecker;
    } else {
        delete cgenRaw;
        delete sdChecker;
        delete hdChecker;
        // s2vChecker is null in library mode
    }

    return result;
}

}  // namespace P4::P4StateDependency
