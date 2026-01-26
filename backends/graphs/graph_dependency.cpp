/**
 * @author Jiwon Kim
 */

#include "graph_dependency.h"

namespace P4::graphs {
std::vector<Graphs::vertex_t> GraphDependency::get_vertices_per_type(Graph *g, VertexType type, bool isStateful) {
    auto vertices = boost::vertices(*g);
    std::vector<Graphs::vertex_t> found_vertices;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (vinfo.isStateful == isStateful) {
            if (type == VertexType::EMPTY || vinfo.type == type) {
                found_vertices.push_back(*vit);
            }
        }
    }
    return found_vertices;
}

// find path from tv to sv
std::vector<Graphs::vertex_t> GraphDependency::find_path_from_vertices(Graph *g, Graphs::vertex_t &sv, Graphs::vertex_t &dv) {
    if (sv == dv)
        return {sv};

    // allocate successor and distance arrays indexed by vertex_index
    std::size_t n = num_vertices(*g);
    std::vector<Graphs::vertex_t> succ(n);

    auto index = boost::get(boost::vertex_index, *g);
    auto succ_map = boost::make_iterator_property_map(
        succ.begin(), index, Graphs::vertex_t());
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        succ_map[*vit] = *vit;
    }

    // Run BFS
    std::vector<bool> visited(n, false);
    std::queue<Graphs::vertex_t> q;
    std::map<Graphs::vertex_t, PathVariables> path_vars;

    visited[index[dv]] = true;
    q.push(dv);
    bool found = false;
    while (!q.empty() && !found) {
        Graphs::vertex_t u = q.front();
        q.pop();

        auto [ei, ei_end] = boost::in_edges(u, *g);
        for (; ei != ei_end; ++ei) {
            Graphs::vertex_t v = boost::source(*ei, *g);
            auto vid = index[v];
            if (!visited[vid]) {
                visited[vid] = true;
                succ_map[v] = u;
                if (v == sv) {
                    found = true;
                    break;          // we can stop early on first reach of d
                }
                q.push(v);
            }
        }
    }
    std::vector<Graphs::vertex_t> path;
    for (Graphs::vertex_t v = sv; ; v = succ_map[v]) {
        path.push_back(v);
        if (v == dv) break;
        if (v == succ_map[v]) return {};
    }
    return path;
}

void GraphDependency::process(std::vector<Graph *> &controlGraphsArray) {
    for (auto g : controlGraphsArray) {
        auto stateful_vertices = get_vertices_per_type(g, VertexType::EMPTY, true);
        auto table_vertices = get_vertices_per_type(g, VertexType::TABLE, false);

        int num_paths = 0;
        for (auto sv : stateful_vertices) {
            for (auto tv : table_vertices) {
                auto path = find_path_from_vertices(g, tv, sv);
                if (path.size() == 0) continue;
                num_paths ++;
                std::cout << (*g)[tv].name << "->" << (*g)[sv].name << " (" << path.size() << ")" << std::endl;
            }
        }
        std::cout << "Number of Stateful: " << stateful_vertices.size()
            << ", and Table: " << table_vertices.size()
            << " Paths: " << num_paths << std::endl;
    }
}
}  // namespace P4::graphs
