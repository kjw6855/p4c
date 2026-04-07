#include "hdr_to_stateful.h"

#include <regex>
#include "graphs.h"

namespace P4::P4StateDependency {

void FindHdrToStateful::set_edge_func_in_graph(Tabulation *tab) {
    auto sgProp = tab->sgProp;
    g = tab->g; // XXX: Init for on-the-fly edge creation

    auto mainProcName = sgProp->procOf[sgProp->rootVar];
    auto rootCFGVit = get_root_vertex(g);      // Find the first node
    std::vector<TabVertex> targetVars;
    // 1. Find all header variables and create edge from root to them
    for (size_t i = 1; i < sgProp->progVarInfo.get_var_num(); i++) {
        auto varVit = sgProp->progVarInfo[rootCFGVit][i];
        auto tabTv = tab->get_tab_vertex(varVit);
        auto varInfo = (*g)[varVit];
        // Skip non-header variables
        // TODO: consider different name for header variables instead of "hdr"
        if (varInfo.name.startsWith("hdr") || (sgProp->ingressPortVar && sgProp->ingressPortVar->equiv(*tabTv.var))) {
            targetVars.push_back(tabTv);
            // on-the-fly create IFDS_FT edge
            // These edges will be removed
            add_edge(sgProp->rootVar, varVit, cstring::empty, EdgeType::IFDS_FT);
        }
    }

    // 2-1. Create map from tableName to its key variable indices
    hvec_map<cstring, std::vector<size_t>> tableKeyVars;
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (!hasFlag(vinfo.flags, VertexFlags::TABLE) ||
                !hasFlag(vinfo.flags, VertexFlags::ENTRY))
            continue;

        auto tableName = sgProp->procOf[*vit];
        auto keyOpt = get_table_key(tab, *vit);
        if (!keyOpt.has_value()) continue;
        auto keyVit = keyOpt.value();
        auto keyInfo = (*g)[keyVit];
        for (auto kv : keyInfo.useVars) {
            auto varIdx = sgProp->progVarInfo.get_var_index(kv, tableName); // Check if kv is a valid variable of the table
            tableKeyVars[tableName].push_back(varIdx);
        }
    }

    // 2-2. Create additional IFDS edges from match keys to defVars in action
    std::size_t n = num_vertices(*g);
    auto index = boost::get(boost::vertex_index, *g);
    std::vector<bool> visited(n, false);
    std::queue<Graphs::vertex_t> q;

    q.push(get_root_vertex(g));
    while (!q.empty()) {
        auto u = q.front();
        q.pop();

        for (auto [ei, ei_end] = boost::out_edges(u, *g); ei != ei_end; ++ei) {
            auto &edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR) continue;
            if (edge.type == EdgeType::IFDS) continue;
            if (edge.type == EdgeType::IFDS_FT) continue;
            auto v = boost::target(*ei, *g);
            auto vid = index[v];
            auto vinfo = (*g)[v];
            auto procName = sgProp->procOf[v];
            auto procSrcit = sgProp->srcOf[procName];
            auto procSrcInfo = (*g)[procSrcit];
            if (!hasFlag(procSrcInfo.flags, VertexFlags::ACTION)) {
                if (!visited[vid]) {
                    visited[vid] = true;
                    q.push(v);
                }
                continue;
            }

            // XXX: Skip input actionVars since they are defined from control plane
            //      (i.e. <u, 0> -> <v, defVar>)
            if (vinfo.name.startsWith("INPUT: ")) {
                if (!visited[vid]) {
                    visited[vid] = true;
                    q.push(v);
                }
                continue;
            }

            for (auto defVar : vinfo.defVars) {
                auto defVarId = sgProp->progVarInfo.get_var_index(defVar, procName);
                auto defVarVit = sgProp->progVarInfo[v][defVarId];

                // Found the action statement
                // Find the match key of the caller table
                auto tableNames = get_tables_from_action(tab, procSrcit);
                for (auto tableName : tableNames) {
                    // Create IFDS edge from <u, kv> to <v, defVar>
                    for (auto keyVarIdx : tableKeyVars[tableName]) {
                        auto keyVit = sgProp->progVarInfo[u][keyVarIdx];
                        // Create edge from match key to action defVar
                        add_edge(keyVit, defVarVit, cstring::empty, EdgeType::IFDS);
                        tempEdges.push_back({keyVit, defVarVit});
                    }
                }
            }

            if (!visited[vid]) {
                visited[vid] = true;
                q.push(v);
            }
        }
    }
    tab->init_edge_func(targetVars);
}

void FindHdrToStateful::analyze_control_graph(Tabulation *tab) {
    g = tab->g; // Init for on-the-fly edge creation
    auto *sgProp = tab->sgProp;
    auto graphName = boost::get_property(*g, boost::graph_name);
    if (sgProp->actionParams.size() == 0) {
        std::cout << "No action params in " << graphName << std::endl;
        return;
    }

    BUG_CHECK(tab->sanity_check_ide(), "Invalid ESG for IDE");
    set_edge_func_in_graph(tab);

    // TODO: propagate match keys to actions
    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();
    //tab->dump_result();

    std::cout << "================" << std::endl;
    std::cout << "[RESULT] Headers -> Stateful Variables in " << graphName << ":\n";
    auto vertices = boost::vertices(*g);
    // Dependency cases for each dst vertex
    hvec_map<Graphs::vertex_t, cstring> caseStrings;
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

        // Check dependencies
        if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
            // Check ACTION->IDX
            cstring caseString = cstring::empty;

            // TODO: differentiate stateful CALL and procedures
            // TODO: apply block could be used only for read
            if (!hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
                    hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {    //TODO: CREATE
                caseString = "[B1/3:->I] "_cs;
            } else if (hasFlag(vinfo.flags, VertexFlags::STATEFUL) &&
                    hasSOFlag(vinfo.soFlags, SOFlags::READ)) {
                caseString = "[->I] "_cs;
            }
            if (caseString.size() == 0) continue;

            caseStrings[*vit] = caseString;
            collect_all_dep_edges(tab, *vit, false);

        } else if (hasFlag(vinfo.flags, VertexFlags::SO_DATA)) {
            // Check ACTION->DATA
            cstring caseString = cstring::empty;

            // TODO: differentiate stateful CALL and procedures
            if (!hasFlag(vinfo.flags, VertexFlags::STATEFUL)) {
                // Assuming it's procedure statements
                caseString = "[B3:->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {
                caseString = "[B3:->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::CREATE)) {
                caseString = "[B2:->D] "_cs;
            }
            if (caseString.size() == 0) continue;

            caseStrings[*vit] = caseString;
            collect_all_dep_edges(tab, *vit, false);
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
        auto &dst = de.first;   // SO_IDX or SO_DATA vertex
        std::stringstream sstream;
        sstream << "[HDR->] " << caseStrings[dst.first]
                << tab->dump_tab_vertex(TabVertex{dst.first, dst.second}) << ": ";

        bool init = true;
        for (auto src : de.second) {
            if (init) init = false;
            else sstream << ", ";
            sstream << src.second;
        }
        std::cout << sstream.str() << std::endl;
    }

    // Remove <0, 0> -> <rootCFG, var> except <rootCFG, 0>
    auto rootCFGVit = get_root_vertex(g);
    for (size_t i = 1; i < sgProp->progVarInfo.get_var_num(); i++) {
        auto varVit = sgProp->progVarInfo[rootCFGVit][i];
        auto edgeIt = boost::edge(sgProp->rootVar, varVit, g->root());
        if (edgeIt.second) boost::remove_edge(edgeIt.first, g->root());
    }
    for (auto &e : tempEdges) {
        auto edgeIt = boost::edge(e.first, e.second, g->root());
        if (edgeIt.second) boost::remove_edge(edgeIt.first, g->root());
    }
    tempEdges.clear();
    tab->clear_edge_func();
    std::cout << "================" << std::endl << std::endl;
}

Visitor::profile_t FindHdrToStateful::init_apply(const IR::Node *n) {
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
