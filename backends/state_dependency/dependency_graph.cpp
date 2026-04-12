#include "dependency_graph.h"

#include <deque>

#include "graphs.h"
#include "lib/nullstream.h"

namespace P4::P4StateDependency {
using vertex_t = DependencyGraphs::vertex_t;
using edge_t = DependencyGraphs::edge_t;

DependencyGraphs::DependencyGraphs(size_t numGraphs) {
    for (size_t i = 0; i < numGraphs; ++i) {
        depGraphs.emplace_back(std::make_unique<DepGraph>());
    }
    nodeToVertexMaps.resize(numGraphs);
    idToVertexMaps.resize(numGraphs);
    leaves.resize(numGraphs);
}

vertex_t DependencyGraphs::add_vertex(size_t index, std::optional<Graphs::vertex_t> nodeId,
                                      const cstring &name, const IR::Node *nodePtr) {
    // Check if vertex with this name already exists
    auto &idToVertexMap = idToVertexMaps[index];
    auto &nodeToVertexMap = nodeToVertexMaps[index];

    if (nodeId.has_value()) {
        if (idToVertexMap.find(nodeId.value()) != idToVertexMap.end() &&
            nodeToVertexMap.find(nodePtr) != nodeToVertexMap.end()) {
            return idToVertexMap[nodeId.value()];
        }
    } else {
        auto it = nodeToVertexMap.find(nodePtr);
        if (it != nodeToVertexMap.end()) {
            return it->second;
        }
    }

    // Create new vertex
    vertex_t v = boost::add_vertex(*depGraphs[index]);

    // Set vertex properties
    DependencyVertex &vData = (*depGraphs[index])[v];
    vData.name = name;
    vData.node = nodePtr;
    vData.color = "lightblue"_cs;
    vData.shape = "box"_cs;

    // Add to lookup maps
    if (nodeId.has_value()) {
        vData.graphId = nodeId.value();
        idToVertexMap[nodeId.value()] = v;
    }

    if (nodePtr) {
        nodeToVertexMap[nodePtr] = v;
    }

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
        ss << (*graph)[srcNode].name << "(" << srcNode << ")";
        ss << ":" << srcVar;

        vertex_t srcVertex = add_vertex(index, srcNode, cstring(ss), srcVar);
        BUG_CHECK(leaves[index].empty() || std::find(leaves[index].begin(), leaves[index].end(), srcVertex) == leaves[index].end(),
                  "Source vertex %1% cannot be a leaf", srcInfo.name);

        // Process all target vertices
        for (const auto &dstVarVertex : dstVarVertices) {
            const auto &[dstNode, dstVar] = dstVarVertex;

            // Get or create destination vertex
            ss.str("");  // Clear the stringstream for reuse
            ss.clear();
            ss << (*graph)[dstNode].name << "(" << dstNode << ")";
            ss << ":" << dstVar;

            vertex_t dstVertex = add_vertex(index, dstNode, cstring(ss), dstVar);

            if (hasLeaves) {
                leaves[index].push_back(dstVertex);
            }

            // Add dependency edge: dstVertex depends on srcVertex
            add_dependency_edge(index, srcVertex, dstVertex, "depends_on"_cs);

            const auto &dstInfo = (*graph)[dstNode];
            if (hasFlag(srcInfo.flags, VertexFlags::SO_IDX) &&
                    hasFlag(dstInfo.flags, VertexFlags::SO_DATA) &&
                    dstInfo.statefulObjectNode != nullptr) {
                BUG_CHECK(!hasLeaves,
                    "Unexpected leaf vertex for stateful object data dependency: %1%",
                    dstInfo.statefulObjectNode);
                // Find its stateful object
                ss.str("");
                ss.clear();
                ss << "[SO] " << dstInfo.statefulObjectNode;
                vertex_t soVertex = add_vertex(index, std::nullopt,
                    cstring(ss), dstInfo.statefulObjectNode);
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

void DependencyGraphs::prune_nodes_not_reaching_leaves(size_t index) {
    auto &g = *depGraphs[index];
    const auto vertexCount = boost::num_vertices(g);
    if (vertexCount == 0) return;

    // Keep every node that can reach at least one leaf (node with out-degree 0).
    std::vector<bool> keep(vertexCount, false);
    std::deque<vertex_t> work;

    for (auto vit : leaves[index]) {
        auto leafInfo = g[vit];
        BUG_CHECK(boost::out_degree(vit, g) == 0,
                  "Expected leaf vertex %1% to have out-degree %2%",
                  leafInfo.name, boost::out_degree(vit, g));
        keep[vit] = true;
        work.push_back(vit);
    }

    while (!work.empty()) {
        auto v = work.front();
        work.pop_front();
        for (auto [eit, eend] = boost::in_edges(v, g); eit != eend; ++eit) {
            auto pred = boost::source(*eit, g);
            if (!keep[pred]) {
                keep[pred] = true;
                work.push_back(pred);
            }
        }
    }

    bool allKept = true;
    for (bool k : keep) {
        if (!k) {
            allKept = false;
            break;
        }
    }
    if (allKept) return;

    auto pruned = std::make_unique<DepGraph>();
    auto oldVAttrs = boost::get(boost::vertex_attribute, g);
    auto newVAttrs = boost::get(boost::vertex_attribute, *pruned);

    std::vector<vertex_t> remap(vertexCount, vertex_t());
    std::vector<bool> remapped(vertexCount, false);

    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto ov = *vit;
        if (!keep[ov]) continue;

        auto nv = boost::add_vertex(*pruned);
        (*pruned)[nv] = g[ov];
        newVAttrs[nv] = oldVAttrs[ov];
        remap[ov] = nv;
        remapped[ov] = true;
    }

    auto oldEAttrs = boost::get(boost::edge_attribute, g);
    auto newEAttrs = boost::get(boost::edge_attribute, *pruned);
    for (auto [eit, eend] = boost::edges(g); eit != eend; ++eit) {
        auto e = *eit;
        auto os = boost::source(e, g);
        auto ot = boost::target(e, g);
        if (!remapped[os] || !remapped[ot]) continue;

        auto [ne, inserted] = boost::add_edge(remap[os], remap[ot], *pruned);
        BUG_CHECK(inserted, "Failed to copy dependency edge during pruning");
        (*pruned)[ne] = g[e];
        newEAttrs[ne] = oldEAttrs[e];
    }

    depGraphs[index] = std::move(pruned);

    // Remap maps except for leaves
    auto &idToVertexMap = idToVertexMaps[index];
    auto &nodeToVertexMap = nodeToVertexMaps[index];
    idToVertexMap.clear();
    nodeToVertexMap.clear();
    for (auto [vit, vend] = boost::vertices(*depGraphs[index]); vit != vend; ++vit) {
        const auto &vData = (*depGraphs[index])[*vit];
        if (vData.graphId.has_value()) idToVertexMap[vData.graphId.value()] = *vit;
        if (vData.node != nullptr) nodeToVertexMap[vData.node] = *vit;
    }
}

}  // namespace P4::P4StateDependency
