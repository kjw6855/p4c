#include "dependency_graph.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "graphs.h"
#include "lib/nullstream.h"
#include "utils.h"

namespace P4::P4StateDependency {
using vertex_t = DependencyGraphs::vertex_t;
using edge_t = DependencyGraphs::edge_t;

// XXX: this value can conflict with __START__ node id
vertex_t globalVertexId = 0;

DependencyGraphs::DependencyGraphs(size_t numGraphs) {
    for (size_t i = 0; i < numGraphs; ++i) {
        depGraphs.emplace_back(std::make_unique<DepGraph>());
    }
    esgToDepMaps.resize(numGraphs);
    leaves.resize(numGraphs);
}

vertex_t DependencyGraphs::add_vertex(size_t index, EsgId esgId, const cstring &name,
                                      const cstring &color) {
    // Check if vertex with this name already exists
    auto &esgToDepMap = esgToDepMaps[index];

    auto it = esgToDepMap.find(esgId);
    if (it != esgToDepMap.end()) {
        return it->second;
    }

    // Create new vertex
    vertex_t v = boost::add_vertex(*depGraphs[index]);

    // Set vertex properties
    DependencyVertex &vData = (*depGraphs[index])[v];
    vData.name = name;
    vData.esgId = esgId;
    vData.color = color;
    vData.shape = "box"_cs;

    // Add to lookup maps
    esgToDepMap[esgId] = v;

    return v;
}

vertex_t DependencyGraphs::add_so_vertex(size_t index, const IR::Node *soNode) {
    std::stringstream ss;
    if (soNode->is<IR::Declaration_Instance>()) {
        ss << "[SO] " << soNode->to<IR::Declaration_Instance>()->name.name;
    } else {
        ss << "[SO] " << soNode;
    }
    vertex_t v = add_vertex(index, {globalVertexId, soNode}, cstring(ss), "lightblue"_cs);
    (*depGraphs[index])[v].isSO = true;
    return v;
}

edge_t DependencyGraphs::add_dependency_edge(size_t index, vertex_t from, vertex_t to,
                                             const cstring &label) {
    // Check if edge already exists
    auto [e, exists] = boost::edge(from, to, *depGraphs[index]);
    if (exists) {
        return e;
    }

    // Create new edge
    auto [newEdge, insertedOK] = boost::add_edge(from, to, *depGraphs[index]);
    BUG_CHECK(insertedOK, "Failed to add dependency edge from vertex %zu to %zu",
                static_cast<size_t>(from), static_cast<size_t>(to));

    // Set edge properties
    DependencyEdge &eData = (*depGraphs[index])[newEdge];
    eData.label = label;
    eData.style = "solid"_cs;

    return newEdge;
}

void DependencyGraphs::add_dependencies_from_map(size_t index, Graphs::Graph *graph,
                                                 const IDEPass::DepEdgeMap &depEdges,
                                                 bool hasLeaves) {
    BUG_CHECK(graph, "Cannot add dependencies: graph pointer is null");

    // Iterate through all dependency edges
    std::stringstream ss;
    for (const auto &[srcVarVertex, dstVarVertices] : depEdges) {
        const auto &[srcNode, srcVar] = srcVarVertex;
        const auto &srcInfo = (*graph)[srcNode];

        // Get or create source vertex
        ss.str("");  // Clear the stringstream for reuse
        ss.clear();
        ss << (*graph)[srcNode].name << "(" << srcNode << "):";
        ss << srcVar;

        vertex_t srcVertex = add_vertex(index, {srcNode, srcVar}, cstring(ss));

        // Process all target vertices
        for (const auto &dstVarVertex : dstVarVertices) {
            const auto &[dstNode, dstVar] = dstVarVertex;

            // Get or create destination vertex
            ss.str("");  // Clear the stringstream for reuse
            ss.clear();
            ss << (*graph)[dstNode].name << "(" << dstNode << "):";
            ss << dstVar;
            vertex_t dstVertex = add_vertex(index, {dstNode, dstVar}, cstring(ss));
            if (hasLeaves) {
                leaves[index].push_back(dstVertex);
            }

            // Add dependency edge: dstVertex depends on srcVertex
            const auto &dstInfo = (*graph)[dstNode];
            if (hasFlag(srcInfo.flags, VertexFlags::CALL) && hasFlag(dstInfo.flags, VertexFlags::RETURN)) {
                // Draw CFG
                std::queue<vertex_t> q; // depGraph
                std::unordered_set<Graphs::vertex_t> visited;   // original ESG
                q.push(srcVertex);
                visited.insert(srcNode);
                bool hasUpdate = false;
                bool endOfSearch = false;
                const IR::Node *regVar = nullptr;
                while (!q.empty()) {
                    vertex_t curVertex = q.front();
                    q.pop();
                    const auto &vInfo = (*depGraphs[index])[curVertex];
                    // 1. Search if next cfg node contains dstNode or not
                    auto nextCfgNodes = find_next_cfg_node(graph, vInfo.esgId.first, false);
                    if (curVertex != srcVertex) {
                        for (auto dEsgit : nextCfgNodes) {
                            if (dEsgit == dstNode) {
                                endOfSearch = true;
                                break;
                            }
                        }
                    }

                    // 2. Run BFS for next cfg nodes
                    for (auto dEsgit : nextCfgNodes) {
                        if (dEsgit == dstNode) {
                            // Don't create CALL-RET edge
                            if (curVertex != srcVertex) {
                                add_dependency_edge(index, curVertex, dstVertex, ""_cs);
                            }
                            break;
                        } else if (endOfSearch) {
                            // Don't create edge for nodes after finding dstNode
                            break;
                        }

                        const auto dEsgInfo = (*graph)[dEsgit];
                        const auto depMapIt = esgToDepMaps[index].find({dEsgit, nullptr});
                        vertex_t dVertex;
                        if (visited.count(dEsgit) && depMapIt != esgToDepMaps[index].end()) {
                            // Get the vertex for this ESG node
                            dVertex = depMapIt->second;
                        } else {
                            ss.str("");
                            ss.clear();
                            ss << dEsgInfo.name << "(" << dEsgit << ")";
                            // TODO: find variable for better visualization
                            dVertex = add_vertex(index, {dEsgit, nullptr}, cstring(ss));

                            // Check if this node updates the stateful object
                            if (std::find(dEsgInfo.defVars.begin(), dEsgInfo.defVars.end(), regVar)
                                    != dEsgInfo.defVars.end()) {
                                hasUpdate = true;
                            }
                            q.push(dVertex);
                            visited.insert(dEsgit);
                        }
                        add_dependency_edge(index, curVertex, dVertex, ""_cs);

                        // Set regVar to determine if stateful object is updated or not
                        if (dEsgInfo.name.startsWith("INPUT: "))
                            regVar = dEsgInfo.defVars.empty() ? nullptr : dEsgInfo.defVars[0];

                        // Add SO edges if the node is stateful
                        if (hasFlag(dEsgInfo.flags, VertexFlags::STATEFUL)) {
                            // Don't create edge for ENTRY/EXIT nodes of control blocks
                            if (hasFlag(dEsgInfo.flags, VertexFlags::ENTRY)) {
                                vertex_t soVertex = add_so_vertex(index, dEsgInfo.statefulObjectNode);
                                add_dependency_edge(index, soVertex, dVertex, "read_from"_cs);
                            }
                            if (hasFlag(dEsgInfo.flags, VertexFlags::EXIT) && hasUpdate) {
                                vertex_t soVertex = add_so_vertex(index, dEsgInfo.statefulObjectNode);
                                add_dependency_edge(index, dVertex, soVertex, "write_to"_cs);
                            }
                        }
                    }
                }
            } else {
                cstring edgeLabel = "depends_on"_cs;
                if (hasFlag(dstInfo.flags, VertexFlags::SO_IDX))
                    edgeLabel = "idx"_cs;
                else if (hasFlag(dstInfo.flags, VertexFlags::SO_DATA)) {
                    if (hasFlag(srcInfo.flags, VertexFlags::SO_IDX)) {
                        edgeLabel = ""_cs;
                    } else {
                        edgeLabel = "data"_cs;
                    }
                }
                add_dependency_edge(index, srcVertex, dstVertex, edgeLabel);
            }

            if (hasFlag(srcInfo.flags, VertexFlags::SO_IDX) &&
                    hasFlag(dstInfo.flags, VertexFlags::SO_DATA) &&
                    dstInfo.statefulObjectNode != nullptr) {
                BUG_CHECK(!hasLeaves,
                    "Unexpected leaf vertex for stateful object data dependency: %1%",
                    dstInfo.statefulObjectNode);
                    // Find its stateful object
                    vertex_t soVertex = add_so_vertex(index, dstInfo.statefulObjectNode);
                if (hasSOFlag(dstInfo.soFlags, SOFlags::READ)) {
                    add_dependency_edge(index, soVertex, dstVertex, "read_from"_cs);
                }
                if (hasSOFlag(dstInfo.soFlags, SOFlags::UPDATE)) {
                    add_dependency_edge(index, dstVertex, soVertex, "write_to"_cs);
                }
                if (hasSOFlag(dstInfo.soFlags, SOFlags::CREATE)) {
                    add_dependency_edge(index, dstVertex, soVertex, "create"_cs);
                }
            }
        }
    }
}

void DependencyGraphs::export_to_graphviz(size_t index, const std::filesystem::path &filepath) const {
    auto out = openFile(filepath, false);
    if (out == nullptr) {
        ::P4::error(ErrorType::ERR_IO, "Failed to open file %1%", filepath);
        return;
    }

    // Write the graph to file in DOT format
    GraphAttributeSetter()(*depGraphs[index]);
    boost::write_graphviz(*out, *depGraphs[index]);

    LOG2("Dependency graph exported to " << filepath.string());
}

void DependencyGraphs::set_vertex_attributes(size_t index, vertex_t v, const cstring &color,
                                           const cstring &shape) {
    DependencyVertex &vData = (*depGraphs[index])[v];
    vData.color = color;
    vData.shape = shape;
}

size_t DependencyGraphs::num_vertices(size_t index) const {
    return boost::num_vertices(*depGraphs[index]);
}

size_t DependencyGraphs::num_edges(size_t index) const {
    return boost::num_edges(*depGraphs[index]);
}

void DependencyGraphs::merge_nodes_without_variable(size_t index) {
    auto &g = *depGraphs[index];
    // 1. Collect ESG vertex with and without variables
    // ESG vertex -> Dependency graph vertex mapping
    hvec_map<Graphs::vertex_t, vertex_t> noVar;
    hvec_map<Graphs::vertex_t, std::vector<vertex_t>> withVar;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto &info = g[*vit];
        if (info.esgId.second == nullptr)
            noVar[info.esgId.first] = *vit;
        else
            withVar[info.esgId.first].push_back(*vit);
    }

    // 2. Define lambda to move edges from one vertex to another
    auto moveEdges = [&](vertex_t from, vertex_t to) {
        for (auto [eit, eend] = boost::in_edges(from, g); eit != eend; ++eit)
            add_dependency_edge(index, boost::source(*eit, g), to, g[*eit].label);
        for (auto [eit, eend] = boost::out_edges(from, g); eit != eend; ++eit)
            add_dependency_edge(index, to, boost::target(*eit, g), g[*eit].label);
    };

    // 3. Finally merge nodes without variables into nodes with variables
    for (auto &[esgNode, noVarVertex] : noVar) {
        // If no vertex with variable is found, keep the no-variable vertex as is
        auto varVerticesIt = withVar.find(esgNode);
        if (varVerticesIt == withVar.end()) continue;

        // Move edges of noVarVertex to varVertex and remove noVarVertex
        for (auto varVertex : varVerticesIt->second)
            moveEdges(noVarVertex, varVertex);

        // Simply remove in/out edges for noVarVertex, since it will be removed later in pruning step.
        for (auto [eit, eend] = boost::in_edges(noVarVertex, g); eit != eend; ++eit)
            boost::remove_edge(*eit, g);
        for (auto [eit, eend] = boost::out_edges(noVarVertex, g); eit != eend; ++eit)
            boost::remove_edge(*eit, g);
    }
}

void DependencyGraphs::prune_nodes_not_reaching_leaves(size_t index) {
    auto &g = *depGraphs[index];
    const auto vertexCount = boost::num_vertices(g);
    if (vertexCount == 0) return;

    // Keep every node that can reach at least one leaf (node with out-degree 0).
    std::unordered_set<vertex_t> keep;
    std::deque<vertex_t> work;

    for (auto vit : leaves[index]) {
        auto leafInfo = g[vit];
        BUG_CHECK(boost::out_degree(vit, g) == 0,
                  "Expected leaf vertex %1% to have out-degree %2%",
                  leafInfo.name, boost::out_degree(vit, g));
        keep.insert(vit);
        work.push_back(vit);
    }

    while (!work.empty()) {
        auto v = work.front();
        work.pop_front();
        for (auto [eit, eend] = boost::in_edges(v, g); eit != eend; ++eit) {
            auto pred = boost::source(*eit, g);
            if (!keep.count(pred)) {
                keep.insert(pred);
                work.push_back(pred);
            }
        }
    }

    if (keep.size() == vertexCount) return;

    auto pruned = std::make_unique<DepGraph>();
    auto oldVAttrs = boost::get(boost::vertex_attribute, g);
    auto newVAttrs = boost::get(boost::vertex_attribute, *pruned);

    std::unordered_map<vertex_t, vertex_t> remap;

    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto ov = *vit;
        if (!keep.count(ov)) continue;

        auto nv = boost::add_vertex(*pruned);
        (*pruned)[nv] = g[ov];
        newVAttrs[nv] = oldVAttrs[ov];
        remap[ov] = nv;
    }

    auto oldEAttrs = boost::get(boost::edge_attribute, g);
    auto newEAttrs = boost::get(boost::edge_attribute, *pruned);
    for (auto [eit, eend] = boost::edges(g); eit != eend; ++eit) {
        auto e = *eit;
        auto os = boost::source(e, g);
        auto ot = boost::target(e, g);
        if (!remap.count(os) || !remap.count(ot)) continue;

        auto [ne, inserted] = boost::add_edge(remap[os], remap[ot], *pruned);
        BUG_CHECK(inserted, "Failed to copy dependency edge during pruning");
        (*pruned)[ne] = g[e];
        newEAttrs[ne] = oldEAttrs[e];
    }

    depGraphs[index] = std::move(pruned);

    for (auto &lv : leaves[index]) {
        BUG_CHECK(remap.count(lv), "Leaf vertex not found in pruned graph");
        lv = remap[lv];
    }

    // Remap esgToDepMap
    auto &esgToDepMap = esgToDepMaps[index];

    esgToDepMap.clear();
    for (auto [vit, vend] = boost::vertices(*depGraphs[index]); vit != vend; ++vit) {
        const auto &vData = (*depGraphs[index])[*vit];
        esgToDepMap[vData.esgId] = *vit;
    }
}

size_t DependencyGraphs::count_data_write_sources_reaching_leaves(size_t index) const {
    const auto &g = *depGraphs[index];
    if (boost::num_vertices(g) == 0 || leaves[index].empty()) return 0;

    // Backward reachability from all leaves.
    std::unordered_set<vertex_t> reachable;
    std::deque<vertex_t> work(leaves[index].begin(), leaves[index].end());
    for (auto v : leaves[index]) reachable.insert(v);
    while (!work.empty()) {
        auto v = work.front(); work.pop_front();
        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
            auto pred = boost::source(*ei, g);
            if (!reachable.count(pred)) { reachable.insert(pred); work.push_back(pred); }
        }
    }

    // Count non-SO vertices with an outgoing "write_to" edge to an SO vertex
    // that are themselves backward-reachable from a leaf.
    size_t count = 0;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto v = *vit;
        if (g[v].isSO || !reachable.count(v)) continue;
        for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
            auto tgt = boost::target(*ei, g);
            if (g[*ei].label == "write_to"_cs && g[tgt].isSO) { ++count; break; }
        }
    }
    return count;
}

size_t DependencyGraphs::count_nowrite_so_leaves(size_t index) const {
    const auto &g = *depGraphs[index];
    if (boost::num_vertices(g) == 0 || leaves[index].empty()) return 0;

    // Collect [SO] vertices with NO incoming "write_to" edge.
    std::vector<vertex_t> noWriteSOs;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto v = *vit;
        if (!g[v].isSO) continue;
        bool hasWrite = false;
        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
            if (g[*ei].label == "write_to"_cs) { hasWrite = true; break; }
        }
        if (!hasWrite) noWriteSOs.push_back(v);
    }
    if (noWriteSOs.empty()) return 0;

    // Forward BFS from those SOs; count distinct leaves reachable.
    std::unordered_set<vertex_t> visited;
    std::deque<vertex_t> work(noWriteSOs.begin(), noWriteSOs.end());
    for (auto v : noWriteSOs) visited.insert(v);
    while (!work.empty()) {
        auto v = work.front(); work.pop_front();
        for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
            auto tgt = boost::target(*ei, g);
            if (!visited.count(tgt)) { visited.insert(tgt); work.push_back(tgt); }
        }
    }

    size_t count = 0;
    for (auto lv : leaves[index])
        if (visited.count(lv)) ++count;
    return count;
}

}  // namespace P4::P4StateDependency
