#include "backends/state_dependency/analysis.h"

#include <boost/graph/graph_traits.hpp>

#ifdef ENABLE_GC
#include <gc/gc.h>
#endif

#include "backends/state_dependency/act_param_to_stateful.h"
#include "backends/state_dependency/controls.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/state_dependency/hdr_to_stateful.h"
#include "backends/state_dependency/ide_pass.h"
#include "backends/state_dependency/stateful_to_key.h"
#include "backends/state_dependency/supergraphs.h"
#include "backends/state_dependency/utils.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "frontends/p4/removeParameters.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "ir/pass_manager.h"
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
                &stateVars, &depEdgeMaps, "A2S2V"_cs, KeySinkMode::KEY_AND_HEADER);
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
            a2s2vGraphs->add_so_constant_edges(i, g);
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_a2s2v_dep.dot"));
            a2s2vGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_a2s2v_dep.dot"));
            a2s2vGraphs->prune_nodes_not_reaching_leaves(i);
            a2s2vGraphs->prune_call_nodes_without_return(i, g);
            if (!graphsDir.empty() && a2s2vGraphs->num_vertices(i) > 0)
                a2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_a2s2v_dep.dot"));
        }
    }
    result.a2s2vGraphs = a2s2vGraphs;

    stateVars.clear();
    depEdgeMaps.clear();

    /* II. H2S2(K/V/C) */
    /* II-1. Base H2S2: header variable → stateful object. Shared by all three sink graphs. */
    auto *h2s2kGraphs = new DependencyGraphs(numGraphs);
    auto *h2s2vGraphs = new DependencyGraphs(numGraphs);
    auto *h2s2cGraphs = new DependencyGraphs(numGraphs);
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
            auto sthEdgeMap = convert_dep_edges(htsEdgeMap);
            h2s2kGraphs->add_dependencies_from_map(i, g, sthEdgeMap);
            h2s2kGraphs->add_dependencies_from_map(i, g, stateVarMap);
            h2s2vGraphs->add_dependencies_from_map(i, g, sthEdgeMap);
            h2s2vGraphs->add_dependencies_from_map(i, g, stateVarMap);
            h2s2cGraphs->add_dependencies_from_map(i, g, sthEdgeMap);
            h2s2cGraphs->add_dependencies_from_map(i, g, stateVarMap);
            for (const auto &ve : htsEdgeMap)
                for (const auto &dst : ve.second)
                    LOG2(Graphs::dump_var_edge(g, {ve.first, dst}));
        }
    }
    /* II-2. H2S2K: state object → table match KEY sinks only. */
    {
        Util::ScopedTimer hdrSoToKeyTimer("HDR->SO->KEY");
        StatefulToKey h2s2kPdChecker(refMap, typeMap,
                &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                &stateVars, &depEdgeMaps, "H2S2V"_cs,
                KeySinkMode::KEY_ONLY);
        program->apply(h2s2kPdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            h2s2kGraphs->add_dependencies_from_map(i, g,
                    h2s2kPdChecker.getFoundDepEdges(graphName), true);
        }
    }
    /* II-3. H2S2V: state object → header/port value sinks only. */
    {
        Util::ScopedTimer hdrSoToHdrTimer("HDR->SO->HDR");
        StatefulToKey h2s2vPdChecker(refMap, typeMap,
                &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                &stateVars, &depEdgeMaps, "H2S2V"_cs,
                KeySinkMode::HEADER_ONLY);
        program->apply(h2s2vPdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            h2s2vGraphs->add_dependencies_from_map(i, g,
                    h2s2vPdChecker.getFoundDepEdges(graphName), true);
        }
    }
    result.h2s2kGraphs = h2s2kGraphs;
    result.h2s2vGraphs = h2s2vGraphs;

    /* II-2. H2S2C: header variable → stateful object → conditions */
    {
        Util::ScopedTimer hdrSoToCondTimer("HDR->SO->COND");
        StatefulToCond h2s2cPdChecker(refMap, typeMap,
                &cgen.controlGraphsArray, &sg.graphProps, GenSGMode::FULL,
                &stateVars, &depEdgeMaps, "H2S2C"_cs);
        program->apply(h2s2cPdChecker);
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            h2s2cGraphs->add_dependencies_from_map(i, g,
                    h2s2cPdChecker.getFoundDepEdges(graphName), true);
        }
    }
    result.h2s2cGraphs = h2s2cGraphs;

    /* IV. S2V: stateful object → key/header (binary-only; only runs when graphsDir is set) */
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
                    &stateVars, &depEdgeMaps, "S2V"_cs, KeySinkMode::KEY_AND_HEADER);
            program->apply(*s2vChecker);
        }
    }

    // Merge and prune dep graphs before chain extraction.
    // prune_nodes_not_reaching_leaves must run before any category analysis.
    // Full/merged exports are emitted here when graphsDir is set (before pruning removes nodes).
    for (size_t i = 0; i < numGraphs; i++) {
        auto *g = cgen.controlGraphsArray[i];
        auto graphName = cstring(boost::get_property(*g, boost::graph_name));

        if (!h2s2kGraphs->leaves[i].empty()) {
            h2s2kGraphs->add_so_constant_edges(i, g);
            if (!graphsDir.empty() && h2s2kGraphs->num_vertices(i) > 0)
                h2s2kGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_h2s2k_dep.dot"));
            h2s2kGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && h2s2kGraphs->num_vertices(i) > 0)
                h2s2kGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_h2s2k_dep.dot"));
            h2s2kGraphs->prune_nodes_not_reaching_leaves(i);
            h2s2kGraphs->prune_call_nodes_without_return(i, g);
        }
        if (!h2s2vGraphs->leaves[i].empty()) {
            h2s2vGraphs->add_so_constant_edges(i, g);
            if (!graphsDir.empty() && h2s2vGraphs->num_vertices(i) > 0)
                h2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_h2s2v_dep.dot"));
            h2s2vGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && h2s2vGraphs->num_vertices(i) > 0)
                h2s2vGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_h2s2v_dep.dot"));
            h2s2vGraphs->prune_nodes_not_reaching_leaves(i);
            h2s2vGraphs->prune_call_nodes_without_return(i, g);
        }
        if (!h2s2cGraphs->leaves[i].empty()) {
            h2s2cGraphs->add_so_constant_edges(i, g);
            if (!graphsDir.empty() && h2s2cGraphs->num_vertices(i) > 0)
                h2s2cGraphs->export_to_graphviz(i, graphsDir / (graphName + "_full_h2s2c_dep.dot"));
            h2s2cGraphs->merge_nodes_without_variable(i);
            if (!graphsDir.empty() && h2s2cGraphs->num_vertices(i) > 0)
                h2s2cGraphs->export_to_graphviz(i, graphsDir / (graphName + "_merged_h2s2c_dep.dot"));
            h2s2cGraphs->prune_nodes_not_reaching_leaves(i);
            h2s2cGraphs->prune_call_nodes_without_return(i, g);
        }
    }

    for (size_t i = 0; i < numGraphs; i++) {
        auto *esg = cgen.controlGraphsArray[i];
        auto graphName = cstring(boost::get_property(*esg, boost::graph_name));
        result.noWriteReadChains[graphName] = {};
    }

    // Each graph carries exactly one sink type, so its data-write chains belong wholly to
    // one category — no post-hoc isKey split, and per-graph chain ids stay contiguous.
    // Category 1 (nowrite SO reads) is sink-independent (shares the base H->SO edges), so it
    // is read from the key graph.
    if (h2s2kGraphs) {
        for (size_t i = 0; i < numGraphs; i++) {
            if (h2s2kGraphs->leaves[i].empty()) continue;
            auto *esg = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*esg, boost::graph_name));
            result.noWriteReadChains[graphName].insert(result.noWriteReadChains[graphName].end(),
                h2s2kGraphs->get_nowrite_so_vertices(i, esg).begin(),
                h2s2kGraphs->get_nowrite_so_vertices(i, esg).end());
            result.dataWriteKeyChains[graphName] = h2s2kGraphs->get_data_write_so_chains(i, esg);
        }
    }
    if (h2s2vGraphs) {
        for (size_t i = 0; i < numGraphs; i++) {
            if (h2s2vGraphs->leaves[i].empty()) continue;
            auto *esg = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*esg, boost::graph_name));
            result.noWriteReadChains[graphName].insert(result.noWriteReadChains[graphName].end(),
                h2s2vGraphs->get_nowrite_so_vertices(i, esg).begin(),
                h2s2vGraphs->get_nowrite_so_vertices(i, esg).end());
            result.dataWriteHeaderChains[graphName] = h2s2vGraphs->get_data_write_so_chains(i, esg);
        }
    }
    if (h2s2cGraphs) {
        for (size_t i = 0; i < numGraphs; i++) {
            if (h2s2cGraphs->leaves[i].empty()) continue;
            auto *esg = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*esg, boost::graph_name));
            result.dataWriteCondChains[graphName] = h2s2cGraphs->get_data_write_so_chains(i, esg);
        }
    }

    // Export pruned dep graphs with satellite chain-category nodes (binary mode only).
    // One file per sink type; each graph is independent so add_chain_satellites mutates
    // only its own graph (no clone/snapshot needed).
    if (!graphsDir.empty()) {
        for (size_t i = 0; i < numGraphs; i++) {
            auto *g = cgen.controlGraphsArray[i];
            auto graphName = cstring(boost::get_property(*g, boost::graph_name));
            if (!h2s2kGraphs->leaves[i].empty() && h2s2kGraphs->num_vertices(i) > 0) {
                auto dotPath = graphsDir / (graphName + "_h2s2k_dep.dot");
                // TODO: noWriteReadChains can have both h2s2v and h2s2k
                auto rankPairs = h2s2kGraphs->add_chain_satellites(i,
                    result.noWriteReadChains[graphName],
                    result.dataWriteKeyChains[graphName]);
                h2s2kGraphs->export_to_graphviz(i, dotPath);
                DependencyGraphs::inject_rank_groups(dotPath, rankPairs);
            }
            if (!h2s2vGraphs->leaves[i].empty() && h2s2vGraphs->num_vertices(i) > 0) {
                auto dotPath = graphsDir / (graphName + "_h2s2v_dep.dot");
                // TODO: noWriteReadChains can have both h2s2v and h2s2k
                auto rankPairs = h2s2vGraphs->add_chain_satellites(i,
                    result.noWriteReadChains[graphName],
                    result.dataWriteHeaderChains[graphName]);
                h2s2vGraphs->export_to_graphviz(i, dotPath);
                DependencyGraphs::inject_rank_groups(dotPath, rankPairs);
            }
            if (!h2s2cGraphs->leaves[i].empty() && h2s2cGraphs->num_vertices(i) > 0) {
                auto dotPath = graphsDir / (graphName + "_h2s2c_dep.dot");
                auto rankPairs = h2s2cGraphs->add_chain_satellites(i, {},
                    result.dataWriteCondChains[graphName]);
                h2s2cGraphs->export_to_graphviz(i, dotPath);
                DependencyGraphs::inject_rank_groups(dotPath, rankPairs);
            }
        }
    }

    // In binary mode, return objects the caller needs for CFG visualization.
    // In library mode, free everything and return the OS the IFDS heap pages so the
    // caller's working set (e.g. symbex solver) starts with a clean RSS.
    if (!graphsDir.empty()) {
        result.cfgGraphs = cgenRaw;
        result.sdChecker = sdChecker;
        result.hdChecker = hdChecker;
        result.s2vChecker = s2vChecker;
    } else {
        delete cgenRaw;
        delete sdChecker;
        delete hdChecker;
        // s2vChecker is null in library mode.
        // Free IFDS dep-graphs; callers only need depChainNodes / depChainNodeIds.
        delete a2s2vGraphs;  result.a2s2vGraphs = nullptr;
        delete h2s2kGraphs;  result.h2s2kGraphs = nullptr;
        delete h2s2vGraphs;  result.h2s2vGraphs = nullptr;
        delete h2s2cGraphs;  result.h2s2cGraphs = nullptr;
#ifdef ENABLE_GC
        // Return freed IFDS pages to the OS before the caller starts allocating.
        GC_gcollect_and_unmap();
#endif
    }

    return result;
}

StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  cstring arch,
                                                  bool isv1,
                                                  std::filesystem::path graphsDir) {
    Util::ScopedTimer sdPrepTimer("P4SD-prep");

    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    refMap.setIsV1(isv1);
    IR::ToplevelBlock *toplevel = nullptr;

    // Prep pipeline:
    //   1. TypeChecking         — populate refMap/typeMap so RAP's internal FindActionParameters
    //                             can resolve action invocations correctly.
    //   2. RemoveActionParameters — aligns clone_ids with any target midend that also runs RAP;
    //                             calls ClearTypeMap internally, leaving typeMap stale.
    //   3. TypeChecking         — rebuild typeMap on the post-RAP program before EvaluatorPass.
    //   4. EvaluatorPass        — capture toplevel with a valid typeMap (needed to evaluate
    //                             constructor arguments in the package hierarchy).
    //
    // A2S2V note: after RAP, action-parameter declarations are gone, so sgProp->actionParams
    // is empty and ActParamToStateful returns early with no results.  That is intentional —
    // A2S2V for post-RAP IR requires updating ActParamToStateful to recognise the local-
    // variable initialisation pattern that RAP introduces.
    {
        auto *evaluator = new P4::EvaluatorPass(&refMap, &typeMap);
        PassManager prep;
        prep.setName("P4SD-prep");
        prep.addPasses({
            new P4::TypeChecking(&refMap, &typeMap, true),
            new P4::RemoveActionParameters(&typeMap),
            new P4::TypeChecking(&refMap, &typeMap, true),
            evaluator,
            [&toplevel, evaluator]() { toplevel = evaluator->getToplevelBlock(); },
        });
        program = program->apply(prep);
    }
    if (program == nullptr || toplevel == nullptr || ::P4::errorCount() > 0)
        return {};

    return runStateDependencyAnalysis(program, &refMap, &typeMap, toplevel, arch, graphsDir);
}

}  // namespace P4::P4StateDependency
