#include "hdr_to_stateful.h"

#include <regex>
#include "backends/ifds/graphs.h"

namespace P4::P4StateDependency {

// Seeds: header variables (plus parser-derived metadata and the ingress port) at the CFG root.
std::vector<TabVertex> FindHdrToStateful::get_seeds(Tabulation *tab) {
    auto sgProp = tab->sgProp;
    auto *g = tab->g;

    auto rootCFGVit = get_root_vertex(g);      // Find the first node
    std::vector<TabVertex> targetVars;
    for (size_t i = 1; i < sgProp->progVarInfo.get_var_num(); i++) {
        auto varVit = sgProp->progVarInfo[rootCFGVit][i];
        auto tabTv = tab->get_tab_vertex(varVit);
        auto varInfo = (*g)[varVit];
        // Skip non-header variables. --parser-deps also seeds parser-derived metadata fields as sources.
        if (varInfo.name.startsWith(sgProp->headerVarName) ||
                sgProp->parserMetaSources.count(varInfo.name) ||
                (sgProp->ingressPortVar && sgProp->ingressPortVar->equiv(*tabTv.var))) {
            targetVars.push_back(tabTv);
        }
    }
    return targetVars;
}

void FindHdrToStateful::add_flow_functions(Tabulation *tab, const std::vector<TabVertex> &seeds,
                                           FlowOverlay &overlay) {
    auto sgProp = tab->sgProp;
    g = tab->g;

    // 1. Create edge from root to every header variable: <0, 0> -> <CFGroot, hdr>
    for (const auto &seed : seeds) {
        overlay.add_flow(tab->g, sgProp->rootVar, tab->get_vertex_id(seed), EdgeType::IFDS_FT);
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
            if (sgProp->progVarInfo.is_local(varIdx)) continue; // Skip local variables since they cannot be propagated to callee
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
                if (sgProp->progVarInfo.is_local(defVarId)) continue; // Skip local variables since they cannot be propagated to callee

                // Found the action statement
                // Find the match key of the caller table
                auto tableNames = get_tables_from_action(tab, procSrcit);
                for (auto tableName : tableNames) {
                    // Create IFDS edge from <u, kv> to <v, defVar>
                    for (auto keyVarIdx : tableKeyVars[tableName]) {
                        if (keyVarIdx == defVarId) continue;
                        auto keyVit = sgProp->progVarInfo[u][keyVarIdx];
                        BUG_CHECK(keyVit != 0, "Key variable %1%(%2%) is out of bounds for table %3%",
                                    sgProp->progVarInfo.get_var(keyVarIdx, tableName), keyVarIdx, tableName);
                        // Create edge from match key to action defVar
                        overlay.add_flow(tab->g, keyVit, defVarVit, EdgeType::IFDS);
                    }
                }
            }

            if (!visited[vid]) {
                visited[vid] = true;
                q.push(v);
            }
        }
    }
}

void FindHdrToStateful::analyze_control_graph(Tabulation *tab) {
    g = tab->g;
    auto graphName = boost::get_property(*g, boost::graph_name);

    solve(tab, get_seeds(tab));
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

    // Remove <0, 0> -> <rootCFG, hdr> and match key -> action defVar edges
    flowOverlay.restore();
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
