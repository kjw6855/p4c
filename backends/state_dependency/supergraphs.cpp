#include <queue>
#include <boost/graph/visitors.hpp>

#include "supergraphs.h"
#include "graphs.h"

namespace P4::P4StateDependency {

const IR::PathExpression* get_base(const IR::Expression* e) {
    if (auto m = e->to<IR::Member>()) return get_base(m->expr);
    return e->to<IR::PathExpression>();
}

void SuperGraphs::create_root_var_vertex() {
    curProp->rootVar = add_var_vertex(Graphs::globalNode);
    auto rootNode = get_root_vertex(g);

    // Put procedure name for rootVar
    curProp->procOf.insert({curProp->rootVar,
            curProp->procOf[rootNode]});

    add_edge(curProp->rootVar, curProp->globalVariables[rootNode][0],
            cstring::empty, EdgeType::IFDS_FT);
}

void SuperGraphs::create_var_vertices(const cstring &graphName) {
    auto graphVarIt = graphVars->find(graphName);
    if (graphVarIt == graphVars->end()) BUG("Graph vars are not found: %1%", graphName);
    auto localGraphVars = graphVarIt->second;

    auto callMapIt = callMaps->find(graphName);
    if (callMapIt == callMaps->end()) {
        // No interprocedural calls in graph
        curProp->callMap = Graphs::CallMap();
    } else {
        curProp->callMap = callMapIt->second;
    }

    auto procOfIt = procOfs->find(graphName);
    if (procOfIt == procOfs->end()) BUG("No procOf: %1%", graphName);
    curProp->procOf = procOfIt->second;

    auto procCallerMapIt = procCallerMaps->find(graphName);
    if (procCallerMapIt == procCallerMaps->end()) {
        // No interprocedural calls in graph
        curProp->procCallerMap = Graphs::ProcCallers();
    } else {
        curProp->procCallerMap = procCallerMapIt->second;
    }

    curProp->varNum = 0;

    // Assign indexMap
    curProp->variableList.push_back(Graphs::globalNode);
    curProp->varIndexMap[Graphs::globalNode] = curProp->varNum++;
    for (auto *node : localGraphVars) {
        curProp->variableList.push_back(node);
        curProp->varIndexMap[node] = curProp->varNum++;
    }

    // For each vertex, create VAR vertex ID
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto varNode = add_var_vertex(Graphs::globalNode, *vit);
        curProp->globalVariables[*vit].push_back(varNode);
        for (auto *node : localGraphVars) {
            auto varNode = add_var_vertex(node, *vit);
            curProp->globalVariables[*vit].push_back(varNode);
        }
    }
}

void SuperGraphs::gen_supergraph(Graph *g_, SuperGraphProp *sgProp) {
    // Init
    g = g_;
    curProp = sgProp;

    create_var_vertices(boost::get_property(*g, boost::graph_name));
    create_root_var_vertex();

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
    add_edge(curProp->globalVariables[src][0], curProp->globalVariables[dst][0],
             cstring::empty, EdgeType::IFDS_FT);

    auto &dstInfo = (*g)[dst];
    for (size_t n = 1; n < curProp->varNum; n++) {
        auto *var = curProp->variableList[n];
        // Fall through if var is not newly defined
        if (std::find(dstInfo.defVars.begin(), dstInfo.defVars.end(), var)
                == dstInfo.defVars.end()) {
            add_edge(curProp->globalVariables[src][n], curProp->globalVariables[dst][n],
                     cstring::empty, EdgeType::IFDS_FT);
            continue;
        }

        // Create new edge
        if (hasFlag(dstInfo.flags, VertexFlags::STATEFUL)) {
            // 0 -> DEF (e.g., v = READ(idx))
            add_edge(curProp->globalVariables[src][0], curProp->globalVariables[dst][n],
                     cstring::empty, EdgeType::IFDS);
        } else if (dstInfo.useVars.size() == 0) {
            // 0 -> DEF
            add_edge(curProp->globalVariables[src][0], curProp->globalVariables[dst][n],
                     cstring::empty, EdgeType::IFDS);
        } else {
            // USE -> DEF
            for (auto uv : dstInfo.useVars) {
                auto uvi = curProp->varIndexMap[uv];
                add_edge(curProp->globalVariables[src][uvi], curProp->globalVariables[dst][n],
                         cstring::empty, EdgeType::IFDS);
            }
        }
    }
}

void SuperGraphs::gen_supergraphs() {
    for (auto *cgg : *controlGraphsArray) {
        auto *sgProp = new SuperGraphProp();
        graphProps.push_back(sgProp);
        gen_supergraph(cgg, sgProp);
    }
}

}  // namespace P4::P4StateDependency
