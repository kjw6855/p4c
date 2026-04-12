#include "stateful_to_key.h"

#include "frontends/p4/methodInstance.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "graphs.h"

namespace P4::P4StateDependency {
std::vector<Graphs::vertex_t> get_actions_from_key(Tabulation *tab, Graphs::vertex_t v) {
    auto *g = tab->g;
    auto vinfo = (*g)[v];
    if (!hasFlag(vinfo.flags, VertexFlags::KEY)) return {};

    std::vector<Graphs::vertex_t> actions;
    for (auto [ei, ei_end] = boost::out_edges(v, *g); ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::CONTROL)
            continue;

        auto u = boost::target(*ei, *g);
        auto uinfo = (*g)[u];
        if (hasFlag(uinfo.flags, VertexFlags::ACTION))
            actions.push_back(u);
    }
    return actions;
}

// Call when setting EdgeFunc in graph for visualization
void FindStatefulToKey::set_edge_func_in_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto graphName = boost::get_property(*g, boost::graph_name);

    // Check if stateVars exists and contains this graph
    if (!stateVars || stateVars->find(graphName) == stateVars->end()) {
        LOG3("Warning: No state variables found for graph '" << graphName << "'");
        return;
    }

    LOG3("Using stateVars for graph '" << graphName << "' with " << (*stateVars)[graphName].size() << " variables");
    tab->init_edge_func((*stateVars)[graphName]);
}

void FindStatefulToKey::analyze_control_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto graphName = boost::get_property(*g, boost::graph_name);

    LOG3("Analyzing control graph '" << graphName << "'");

    BUG_CHECK(tab->sanity_check_ide(), "Invalid ESG for IDE");

    // Debug: Log available stateVars keys
    if (stateVars) {
        LOG3("Available stateVars keys:");
        for (const auto& kv : *stateVars) {
            LOG3("  " << kv.first);
        }
    }

    // Check if stateVars exists and contains this graph
    if (!stateVars || stateVars->find(graphName) == stateVars->end()) {
        LOG2("No state variables found for graph '" << graphName << "', skipping analysis");
        return;
    }

    auto curStateVars = (*stateVars)[graphName];
    LOG3("Found " << curStateVars.size() << " state variables for graph '" << graphName << "'");

    if (curStateVars.empty()) {
        std::cout << "No state variables in " << graphName << std::endl;
        return;
    }

    auto prevDepEdgeMap = prevDepEdgeMaps ? (*prevDepEdgeMaps)[graphName] : IDEPass::DepEdgeMap{};

    tab->init_edge_func(curStateVars);
    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();
    //tab->dump_result();

    std::cout << "================" << std::endl;
    std::cout << "[RESULT]";
    if (analysisType == "A2S2V"_cs) std::cout << " Action Parameters ->";
    else if (analysisType == "H2S2V"_cs) std::cout << " Headers ->";
    std::cout << " Stateful Variables -> Headers/Keys in " << graphName << ":\n";
    auto mainProcName = sgProp->procOf[sgProp->rootVar];
    auto vertices = boost::vertices(*g);
    std::array<size_t, 6> numEntities{};
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];
        // Measure number of nodes in the graph
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) {
            //numReachableESGNodes
            if (vinfo.interesting) numEntities[2]++;
            numEntities[1]++;   // numESGNodes
        } else {
            numEntities[0]++;   // numNodes
        }

        // TODO: support for condition blocks (e.g., if and switch)
        if (hasFlag(vinfo.flags, VertexFlags::KEY)) {
            collect_all_dep_edges(tab, *vit);
        }
        if (hasFlag(vinfo.flags, VertexFlags::EXIT) && sgProp->procOf[*vit] == mainProcName) {
            collect_all_dep_edge_to_hdr(tab, *vit);
        }
    }
    auto edges = boost::edges(*g);
    for (auto &eit = edges.first; eit != edges.second; ++eit) {
        auto &edge = (*g)[*eit];
        if (edge.type == EdgeType::IFDS || edge.type == EdgeType::IFDS_FT) {
            auto src = boost::source(*eit, *g);
            auto dst = boost::target(*eit, *g);
            auto srcInfo = (*g)[src];
            auto dstInfo = (*g)[dst];
            // numReachableESGEdge
            if (srcInfo.interesting && dstInfo.interesting) numEntities[5]++;
            numEntities[4]++;   // numESGEdges

        } else if (edge.type != EdgeType::HAS_VAR) {
            numEntities[3]++;   // numEdges
        }
    }

    std::cout << "Total nodes: " << numEntities[0] << std::endl;
    std::cout << "Total edges: " << numEntities[3] << std::endl;
    std::cout << "Total ESG nodes: " << numEntities[1] << std::endl;
    std::cout << "Total ESG edges: " << numEntities[4] << std::endl;
    std::cout << "Reachable ESG nodes: " << numEntities[2] << std::endl;
    std::cout << "Reachable ESG edges: " << numEntities[5] << std::endl;

    for (auto &de : foundDepEdges[graphName]) {
        auto &src = de.first;
        std::stringstream sstream;
        sstream << "[SO->] " << tab->dump_tab_vertex(TabVertex{src.first, src.second});

        auto prevDepEdgeMapIt = prevDepEdgeMap.find(src);
        if (prevDepEdgeMapIt != prevDepEdgeMap.end()) {
            bool init = true;
            for (auto &prevDst : prevDepEdgeMapIt->second) {
                if (init) {
                    init = false;
                    sstream << " ... ";
                } else {
                    sstream << ", ";
                }
                // XXX: simple heuristic not to print CFG node
                if (prevDst.first == 0) sstream << prevDst.second;
                else sstream << tab->dump_tab_vertex(TabVertex{prevDst.first, prevDst.second});
            }
        }
        sstream << "\n";

        for (auto &dst : de.second) {
            auto dstInfo = (*g)[dst.first];
            if (!hasFlag(dstInfo.flags, VertexFlags::KEY)) {
                sstream << "  [->HDR/PORT] " << tab->dump_tab_var_name(TabVertex{dst.first, dst.second}) << "\n";

            } else {
                sstream << "  [->KEY] "
                    << tab->dump_tab_var_name(TabVertex{dst.first, dst.second})
                    << " @TABLE:" << tab->sgProp->procOf[dst.first] << "(";
                bool init = true;
                for (auto actit : get_actions_from_key(tab, dst.first)) {
                    auto actinfo = (*g)[actit];
                    if (init) init = false;
                    else sstream << ", ";

                    if (hasFlag(actinfo.flags, VertexFlags::CALL))
                        sstream << actinfo.name.substr(5);  // "CALL "
                    else
                        sstream << actinfo.name;
                }
                sstream << ")\n";
            }
        }
        std::cout << sstream.str();
    }
    tab->clear_edge_func();
    std::cout << "================" << std::endl << std::endl;
}

Visitor::profile_t FindStatefulToKey::init_apply(const IR::Node *n) {
    foundDepEdges.clear();
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};

        analyze_control_graph(tab);
    }

    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
