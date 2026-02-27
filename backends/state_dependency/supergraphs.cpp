#include <queue>
#include <boost/graph/visitors.hpp>

#include "supergraphs.h"
#include "graphs.h"

namespace P4::P4StateDependency {

SuperGraphs::SuperGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars,
                std::vector<Graph *> *controlGraphsArray)
    : refMap(refMap), typeMap(typeMap), controlGraphsArray(controlGraphsArray), graphVars(graphVars) {}

void SuperGraphs::gen_supergraph(Graph *g_) {
    // Init
    g = g_;
    init_all_variables();

    // Traverse ICFG
    std::size_t n = num_vertices(*g);
    auto index = boost::get(boost::vertex_index, *g);
    std::vector<bool> visited(n, false);
    std::queue<Graphs::vertex_t> q;

    q.push(get_root_vertex(g));
    while (!q.empty()) {
        auto u = q.front();
        q.pop();

        for (auto [ei, ei_end] = boost::out_edges(u, *g); ei != ei_end; ++ei) {
            auto edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR) continue;
            if (edge.type == EdgeType::IFDS) continue;
            auto v = boost::target(*ei, *g);

            // main
            LOG5(u << "->" << v << ": " << edgeTypeToString(edge.type));
            gen_ifds_edge(u, v);

            auto vid = index[v];
            if (!visited[vid]) {
                visited[vid] = true;
                q.push(v);
            }
        }
    }
}

void SuperGraphs::gen_ifds_edge(Graphs::vertex_t src, Graphs::vertex_t dst) {

    // Add 0->0
    add_edge(globalVariables[src][0], globalVariables[dst][0],
             cstring::empty, EdgeType::IFDS_FT);

    auto &dstInfo = (*g)[dst];
    for (size_t n = 1; n < varNum; n++) {
        auto *var = variableList[n];
        // Fall through if var is not newly defined
        if (std::find(dstInfo.defVars.begin(), dstInfo.defVars.end(), var)
                == dstInfo.defVars.end()) {
            add_edge(globalVariables[src][n], globalVariables[dst][n],
                     cstring::empty, EdgeType::IFDS_FT);
            continue;
        }

        if (dstInfo.useVars.size() == 0) {
            add_edge(globalVariables[src][0], globalVariables[dst][n],
                     cstring::empty, EdgeType::IFDS);
        } else {
            for (auto uv : dstInfo.useVars) {
                auto uvi = varIndexMap[uv];
                add_edge(globalVariables[src][uvi], globalVariables[dst][n],
                         cstring::empty, EdgeType::IFDS);
            }
        }
    }
}

void SuperGraphs::init_all_variables() {
    variableList.clear();
    varIndexMap.clear();
    globalVariables.clear();

    auto graphName = boost::get_property(*g, boost::graph_name);
    auto graphVarIt = graphVars->find(graphName);
    if (graphVarIt == graphVars->end()) BUG("Graph vars are not found: %1%", graphName);

    auto localGraphVars = graphVarIt->second;
    varNum = 0;

    // Assign indexMap
    variableList.push_back(Graphs::globalNode);
    varIndexMap[Graphs::globalNode] = varNum++;
    for (auto *node : localGraphVars) {
        variableList.push_back(node);
        varIndexMap[node] = varNum++;
    }

    // For each vertex, set VAR vertex ID
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto varNode = add_var_vertex(Graphs::globalNode, *vit);
        globalVariables[*vit].push_back(varNode);
        for (auto *node : localGraphVars) {
            auto varNode = add_var_vertex(node, *vit);
            globalVariables[*vit].push_back(varNode);
        }
    }
}

bool SuperGraphs::preorder(const IR::PackageBlock *block) {
    for (auto it : block->constantValue) {
        if (!it.second) continue;
        if (it.second->is<IR::ControlBlock>()) {
            auto name = it.second->to<IR::ControlBlock>()->container->name;
            for (auto *cgg : *controlGraphsArray) {
                auto cggName = boost::get_property(*cgg, boost::graph_name);
                if (cggName != name.string()) continue;

                // FOUND
                gen_supergraph(cgg);
            }
        } else if (it.second->is<IR::PackageBlock>()) {
            visit(it.second->getNode());
        }
    }
    return false;
}

}  // namespace P4::P4StateDependency
