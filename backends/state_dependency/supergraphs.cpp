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
             cstring::empty, EdgeType::IFDS);

    // TODO...
}

void SuperGraphs::init_all_variables() {
    globalVariables.clear();

    auto graphName = boost::get_property(*g, boost::graph_name);
    auto graphVarIt = graphVars->find(graphName);
    if (graphVarIt == graphVars->end()) BUG("Graph vars are not found: %1%", graphName);

    auto localGraphVars = graphVarIt->second;
    varNum = localGraphVars.size() + 1;
    auto vertices = boost::vertices(*g);

    // For each vertex, add variables
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];
        vinfo.variables.insert(vinfo.variables.end(),
                localGraphVars.begin(), localGraphVars.end());

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
