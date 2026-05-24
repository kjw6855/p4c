#include "dependency_graph.h"

#include <deque>
#include <fstream>
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

// Helper: if a vertex set contains multiple RETURN vertices (dep-graph
// vertices with incoming CALL_TO_RET edges), keep only one RETURN that is
// connected to a CALL vertex present in the same vertex set. If none of the
// RETURN vertices has its CALL inside the set, keep the first RETURN and drop
// the rest. When dropping RETURNs, also drop vertices that are only reachable
// from those removed RETURNs, but keep downstream vertices reachable from the
// retained RETURN.
static void keep_one_return_connected(const DependencyGraphs::DepGraph &g,
                                     std::unordered_set<vertex_t> &vtx) {
    std::vector<vertex_t> retVerts;
    for (auto v : vtx) {
        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
            if (g[*ei].type == DependencyGraphs::DepEdgeType::CALL_TO_RET) {
                retVerts.push_back(v);
                break;
            }
        }
    }
    if (retVerts.size() <= 1) return;

    // Prefer a RETURN whose CALL predecessor is inside the same vertex set.
    bool found = false;
    vertex_t chosen = retVerts.front();
    for (auto r : retVerts) {
        for (auto [ei, ee] = boost::in_edges(r, g); ei != ee; ++ei) {
            if (g[*ei].type != DependencyGraphs::DepEdgeType::CALL_TO_RET) continue;
            auto callV = boost::source(*ei, g);
            if (vtx.count(callV)) { chosen = r; found = true; break; }
        }
        if (found) break;
    }

    std::unordered_set<vertex_t> keepReachable;
    std::deque<vertex_t> work;
    keepReachable.insert(chosen);
    work.push_back(chosen);

    while (!work.empty()) {
        auto v = work.front();
        work.pop_front();
        for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
            auto succ = boost::target(*ei, g);
            if (vtx.count(succ) && keepReachable.insert(succ).second) {
                work.push_back(succ);
            }
        }
    }

    std::unordered_set<vertex_t> doomed;
    for (auto r : retVerts) {
        if (r == chosen) continue;

        std::deque<vertex_t> dropWork;
        if (doomed.insert(r).second) dropWork.push_back(r);

        while (!dropWork.empty()) {
            auto v = dropWork.front();
            dropWork.pop_front();

            for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
                auto succ = boost::target(*ei, g);
                if (vtx.count(succ) && !keepReachable.count(succ) && doomed.insert(succ).second) {
                    dropWork.push_back(succ);
                }
            }
        }
    }

    for (auto v : doomed) vtx.erase(v);
}

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
        // Use controlPlaneName() so the label matches the key used in the test-object store
        // (e.g. "ingress.roundRegister" from the @name annotation, not just "roundRegister").
        ss << soNode->to<IR::Declaration_Instance>()->controlPlaneName();
    } else {
        ss << soNode;
    }
    vertex_t v = add_vertex(index, {globalVertexId, soNode}, cstring(ss), "lightblue"_cs);
    (*depGraphs[index])[v].isSO = true;
    return v;
}

edge_t DependencyGraphs::add_dependency_edge(size_t index, vertex_t from, vertex_t to,
                                             const cstring &label,
                                             DepEdgeType type) {
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
    eData.type = type;

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
                            } else {
                                add_dependency_edge(index, srcVertex, dstVertex,
                                    "call_to_return"_cs, DepEdgeType::CALL_TO_RET);
                            }
                            break;
                        } else if (endOfSearch) {
                            // dstNode is in nextCfgNodes but not yet reached; skip adding
                            // this node to BFS so we don't expand past the procedure boundary,
                            // but keep iterating to reach dstNode.
                            continue;
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

                            // Check if this node updates the stateful object.
                            // defVar may be a member expression (e.g. val.field = x), so walk
                            // the IR::Member chain to the root PathExpression and compare names.
                            auto isRegVarOrMember = [&](const IR::Node *defVar) -> bool {
                                if (regVar == nullptr) return false;
                                const IR::Node *cur = defVar;
                                while (cur != nullptr) {
                                    if (cur == regVar) return true;
                                    if (const auto *mem = cur->to<IR::Member>()) {
                                        cur = mem->expr;
                                    } else if (const auto *pe = cur->to<IR::PathExpression>()) {
                                        const auto *decl = regVar->to<IR::IDeclaration>();
                                        return decl && pe->path->name == decl->getName();
                                    } else {
                                        break;
                                    }
                                }
                                return false;
                            };
                            if (std::any_of(dEsgInfo.defVars.begin(), dEsgInfo.defVars.end(),
                                            isRegVarOrMember)) {
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

            if (hasFlag(dstInfo.flags, VertexFlags::SO_DATA) &&
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

void DependencyGraphs::add_so_constant_edges(size_t index, Graphs::Graph *esg) {
    auto &g = *depGraphs[index];
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto &vInfo = g[*vit];

        const auto &esgNode = vInfo.esgId.first;
        const auto &esgInfo = (*esg)[esgNode];
        // SO_IDX UPDATE node without any edges
        if (!hasFlag(esgInfo.flags, VertexFlags::SO_IDX)) continue;
        if (!hasSOFlag(esgInfo.soFlags, SOFlags::UPDATE)) continue;
        // Skip if there is any edge (e.g., from IDX to DATA)
        auto [eit, eend] = boost::out_edges(*vit, g);
        if (eit != eend) continue;

        // Add static write edge from vertex to its stateful object
        vertex_t soVertex = add_so_vertex(index, esgInfo.statefulObjectNode);
        add_dependency_edge(index, *vit, soVertex, "constant"_cs);
    }
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
    prune_and_remap(index, keep);
}

void DependencyGraphs::prune_call_nodes_without_return(size_t index, Graphs::Graph *esg) {
    auto &g = *depGraphs[index];
    const auto vertexCount = boost::num_vertices(g);
    if (vertexCount == 0) return;
    std::unordered_set<vertex_t> keep;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto &vInfo = g[*vit];
        const auto &esgInfo = (*esg)[vInfo.esgId.first];
        if (!hasFlag(esgInfo.flags, VertexFlags::CALL)) {
            keep.insert(*vit);
            continue;
        }

        bool hasRetEdge = false;
        for (auto [eit, eend] = boost::out_edges(*vit, g); eit != eend; ++eit) {
            if (g[*eit].type == DepEdgeType::CALL_TO_RET) {
                hasRetEdge = true;
                break;
            }
        }
        if (hasRetEdge) keep.insert(*vit);
    }

    if (keep.size() == vertexCount) return;
    prune_and_remap(index, keep);
}

void DependencyGraphs::prune_and_remap(size_t index, std::unordered_set<vertex_t> &keep) {
    auto &g = *depGraphs[index];
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

std::vector<DependencyGraphs::SOChain>
DependencyGraphs::get_nowrite_so_vertices(size_t index, Graphs::Graph *esg) const {
    const auto &g = *depGraphs[index];
    std::vector<SOChain> chains;
    if (boost::num_vertices(g) == 0 || leaves[index].empty()) return chains;

    // Collect SO vertices with no incoming "write_to" edge.
    std::vector<vertex_t> noWriteSOs;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto v = *vit;
        if (!g[v].isSO) continue;
        bool hasWrite = false;
        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
            if (g[*ei].label == "write_to"_cs || g[*ei].label == "constant"_cs) {
                hasWrite = true;
                break;
            }
        }
        if (!hasWrite) noWriteSOs.push_back(v);
    }
    if (noWriteSOs.empty()) return chains;

    // Find read_from non-write SO vertices
    std::map<vertex_t, std::unordered_set<vertex_t>> readDestinations;
    for (auto so : noWriteSOs) {
        for (auto [ei, ee] = boost::out_edges(so, g); ei != ee; ++ei) {
            if (g[*ei].label == "read_from"_cs)
                readDestinations[so].insert(boost::target(*ei, g));
        }
    }

    size_t chainId = 0;
    for (auto [so, readDsts] : readDestinations) {
        for (auto readDst : readDsts) {
            std::unordered_set<vertex_t> readVtx;
            std::deque<vertex_t> work;

            // Run backward BFS from readDst
            readVtx.insert(readDst);
            work.push_back(readDst);
            while (!work.empty()) {
                auto v = work.front(); work.pop_front();
                for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                    auto src = boost::source(*ei, g);
                    if (src == so) continue; // Don't cross read_from edge back to SO
                    if (!readVtx.count(src)) {
                        readVtx.insert(src);
                        work.push_back(src);
                    }
                }
            }

            // Run forward BFS from readDst
            work.push_back(readDst);
            while (!work.empty()) {
                auto v = work.front(); work.pop_front();
                for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
                    auto tgt = boost::target(*ei, g);
                    if (!readVtx.count(tgt)) {
                        readVtx.insert(tgt);
                        work.push_back(tgt);
                    }
                }
            }
            // If multiple RETURN vertices are present, keep only the one
            // connected to a CALL vertex inside this read-vertex set.
            keep_one_return_connected(g, readVtx);

            for (auto v : readVtx) {
                if (g[v].isSO) continue;
                if (boost::out_degree(v, g) == 0) {
                    chains.push_back(SOChain(depGraphs[index].get(), esg,
                                            so, g[so].name, g[so].esgId.second,
                                            std::unordered_set<vertex_t>(),
                                            readVtx, false, chainId++,
                                            g[v]));
                }
            }
        }
    }
    return chains;
}

std::vector<DependencyGraphs::SOChain>
DependencyGraphs::get_data_write_so_chains(size_t index, Graphs::Graph *esg,
                                          std::unordered_set<cstring> *soNames) const {
    const auto &g = *depGraphs[index];
    std::vector<SOChain> chains;
    if (boost::num_vertices(g) == 0 || leaves[index].empty()) return chains;

    // Find SO vertices with at least one incoming "write_to" edge.
    std::vector<vertex_t> writeSOs;
    for (auto [vit, vend] = boost::vertices(g); vit != vend; ++vit) {
        auto v = *vit;
        if (!g[v].isSO) continue;
        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
            if (g[*ei].label == "write_to"_cs) {
                writeSOs.push_back(v);
                if (soNames) soNames->insert(g[v].name);
                break;
            }
        }
    }
    if (writeSOs.empty()) return chains;

    // Collect distinct write-source vertices (direct non-SO predecessors via write_to).
    std::map<vertex_t, std::unordered_set<vertex_t>> writeSources;
    for (auto so : writeSOs) {
        for (auto [ei, ee] = boost::in_edges(so, g); ei != ee; ++ei) {
            if (g[*ei].label == "write_to"_cs)
                writeSources[so].insert(boost::source(*ei, g));
        }
    }
    BUG_CHECK(!writeSources.empty(), "Expected at least one write source for data-write SOs");

    // Collect distinct read-destination vertices from writeSO (pure read_from only).
    std::map<vertex_t, std::unordered_set<vertex_t>> readDestinations;
    for (auto [so, _] : writeSources) {
        for (auto [ei, ee] = boost::out_edges(so, g); ei != ee; ++ei) {
            if (g[*ei].label == "read_from"_cs) {
                auto tgt = boost::target(*ei, g);
                readDestinations[so].insert(tgt);
            }
        }
    }

    // Helper: find the first leaf dep vertex (out_degree 0, non-SO) in a vertex set
    // and return its DependencyVertex (for sinkNode). Returns default if none found.
    auto findSinkDVs = [&](const std::unordered_set<vertex_t> &vtxSet,
                          const std::unordered_set<vertex_t> *skipSet = nullptr)
                          -> std::vector<DependencyVertex> {
        std::vector<DependencyVertex> sinkDVs;
        for (auto v : vtxSet) {
            if (skipSet && skipSet->count(v)) continue;
            if (g[v].isSO) continue;
            if (boost::out_degree(v, g) == 0) {
                sinkDVs.push_back(g[v]);
            }
        }
        return sinkDVs;
    };

    size_t chainId = 0;
    auto filter_vertices_for_sink = [&](const std::unordered_set<vertex_t> &vtx,
                                        const DependencyVertex &selectedSink,
                                        const std::vector<DependencyVertex> &allSinks)
                                        -> std::unordered_set<vertex_t> {
        if (allSinks.size() <= 1) return vtx;
        std::unordered_set<vertex_t> filtered;
        filtered.reserve(vtx.size());
        for (auto v : vtx) {
            const auto &vertexInfo = g[v];
            bool keep = true;
            for (const auto &sink : allSinks) {
                if (sink.esgId == selectedSink.esgId) continue;
                if (vertexInfo.esgId == sink.esgId) {
                    keep = false;
                    break;
                }
            }
            if (keep) filtered.insert(v);
        }
        return filtered;
    };

    // Get SO chain for each (writeSO, so, readDst) pair
    for (auto [so, sources] : writeSources) {
        // 1. Collect paths for each write source through backward BFS.
        std::map<vertex_t, std::unordered_set<vertex_t>> writeVtx;
        std::map<vertex_t, vertex_t> updateReadDstToWriteSrc; // for detecting update SOs with same read/write vertex
        for (auto src : sources) {
            std::deque<vertex_t> bwWork;
            writeVtx[src].insert(src);
            bwWork.push_back(src);
            while (!bwWork.empty()) {
                auto v = bwWork.front(); bwWork.pop_front();
                for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                    auto tgt = boost::source(*ei, g);
                    if (writeVtx.count(tgt)) continue;

                    // don't cross update-read edge to keep the chain pure
                    if (g[*ei].label == "read_from"_cs && tgt == so) {
                        updateReadDstToWriteSrc[v] = src;
                        continue;
                    }

                    writeVtx[src].insert(tgt);
                    bwWork.push_back(tgt);
                }
            }
        }

        // Helper: split a write-vertex set into per-call-site subsets.
        // When vtx contains >1 CALL vertices (vertices with a CALL_TO_RET out-edge),
        // returns one subset per CALL vertex: procBody (EXIT..ENTRY, shared) plus
        // that CALL vertex and its caller-side backward context within vtx.
        // Returns {vtx} unchanged when there are 0 or 1 CALL vertices.
        auto splitBySite = [&](const std::unordered_set<vertex_t> &vtx, vertex_t start)
            -> std::vector<std::unordered_set<vertex_t>> {
            std::vector<vertex_t> callVerts;
            for (auto v : vtx) {
                for (auto [eo, eoe] = boost::out_edges(v, g); eo != eoe; ++eo) {
                    if (g[*eo].type == DepEdgeType::CALL_TO_RET) {
                        callVerts.push_back(v);
                        break;
                    }
                }
            }
            if (callVerts.size() <= 1) return {vtx};

            std::unordered_set<vertex_t> callVertSet(callVerts.begin(), callVerts.end());

            // Procedure body: backward BFS from start within vtx, stopping at CALL vertices.
            std::unordered_set<vertex_t> procBody;
            std::deque<vertex_t> work;
            procBody.insert(start);
            work.push_back(start);
            while (!work.empty()) {
                auto v = work.front(); work.pop_front();
                for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                    auto pred = boost::source(*ei, g);
                    if (!vtx.count(pred) || procBody.count(pred) || callVertSet.count(pred))
                        continue;
                    procBody.insert(pred);
                    work.push_back(pred);
                }
            }

            // One write-vertex set per CALL vertex: procBody + callV + its backward context.
            std::vector<std::unordered_set<vertex_t>> result;
            for (auto callV : callVerts) {
                auto perSite = procBody;
                perSite.insert(callV);
                std::deque<vertex_t> callWork;
                callWork.push_back(callV);
                while (!callWork.empty()) {
                    auto v = callWork.front(); callWork.pop_front();
                    for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                        auto pred = boost::source(*ei, g);
                        // Stop at the procedure body and at other call-site vertices so that
                        // e.g. CALL₁'s context doesn't bleed into CALL₂'s chain when the
                        // CALL_TO_RET edge on RETURN₁ reaches CALL₁ during CALL₂'s BFS.
                        if (!vtx.count(pred) || perSite.count(pred) || callVertSet.count(pred)) continue;
                        perSite.insert(pred);
                        callWork.push_back(pred);
                    }
                }
                result.push_back(std::move(perSite));
            }
            return result;
        };

        // Helper: split a read-vertex set into per-call-site subsets.
        // Symmetric to splitBySite but uses forward BFS from start (the ENTRY vertex)
        // to discover the shared procedure body, then backward BFS per CALL vertex for
        // its caller-side context.  Returns {vtx} when there are 0 or 1 CALL vertices.
        auto splitReadBySite = [&](const std::unordered_set<vertex_t> &vtx, vertex_t start)
            -> std::vector<std::unordered_set<vertex_t>> {
            // Procedure body = forward BFS from start within vtx.
            std::unordered_set<vertex_t> procBody;
            std::deque<vertex_t> work;
            procBody.insert(start);
            work.push_back(start);
            while (!work.empty()) {
                auto v = work.front(); work.pop_front();
                for (auto [eo, eoe] = boost::out_edges(v, g); eo != eoe; ++eo) {
                    auto tgt = boost::target(*eo, g);
                    if (vtx.count(tgt) && !procBody.count(tgt)) {
                        procBody.insert(tgt);
                        work.push_back(tgt);
                    }
                }
            }

            // CALL vertices are outside the procedure body.
            std::vector<vertex_t> callVerts;
            std::unordered_set<vertex_t> callVertSet;
            for (auto v : vtx) {
                if (procBody.count(v)) continue;
                for (auto [eo, eoe] = boost::out_edges(v, g); eo != eoe; ++eo) {
                    if (g[*eo].type == DepEdgeType::CALL_TO_RET) {
                        callVerts.push_back(v);
                        callVertSet.insert(v);
                        break;
                    }
                }
            }
            if (callVerts.size() <= 1) return {vtx};

            // One read-vertex set per CALL vertex: procBody + callV + its backward context.
            std::vector<std::unordered_set<vertex_t>> result;
            for (auto callV : callVerts) {
                auto perSite = procBody;
                perSite.insert(callV);
                std::deque<vertex_t> callWork;
                callWork.push_back(callV);
                while (!callWork.empty()) {
                    auto v = callWork.front(); callWork.pop_front();
                    for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                        auto pred = boost::source(*ei, g);
                        if (!vtx.count(pred) || perSite.count(pred) || callVertSet.count(pred)) continue;
                        perSite.insert(pred);
                        callWork.push_back(pred);
                    }
                }
                result.push_back(std::move(perSite));
            }
            return result;
        };

        // 2. Collect paths for each read destination through backward and forward BFS.
        //    Cache the already-split per-site sets so the BFS runs only once per readDst.
        std::map<vertex_t, std::vector<std::unordered_set<vertex_t>>> readSitesCache;
        for (auto readDst : readDestinations[so]) {
            // 1) If readDst is for update, create a single-execution path
            if (updateReadDstToWriteSrc.count(readDst)) {
                // Add write-only path for update
                auto writeSrc = updateReadDstToWriteSrc[readDst];
                bool hasUpdatePath = false;
                for (auto [ei, ee] = boost::out_edges(writeSrc, g); ei != ee; ++ei) {
                    auto tgt = boost::target(*ei, g);
                    if (tgt == so && g[*ei].label == "write_to"_cs) continue;
                    hasUpdatePath = true;
                    break;
                }
                // Skip if there is no further path
                if (!hasUpdatePath) continue;

                // Forward BFS from writeSrc
                std::deque<vertex_t> fwWork;
                std::unordered_set<vertex_t> writeLocalVtx;
                writeLocalVtx.insert(writeVtx[writeSrc].begin(), writeVtx[writeSrc].end());
                fwWork.push_back(writeSrc);
                while (!fwWork.empty()) {
                    auto v = fwWork.front(); fwWork.pop_front();
                    for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
                        auto tgt = boost::target(*ei, g);
                        if (g[*ei].label == "write_to"_cs && tgt == so) {
                            // skip crossing update-read edge to keep the chain pure
                            continue;
                        }
                        if (writeLocalVtx.count(tgt)) continue;
                        writeLocalVtx.insert(tgt);
                        fwWork.push_back(tgt);
                    }
                }
                // Clean up unreachable RETURNs in the write-side vertex set.
                keep_one_return_connected(g, writeLocalVtx);
                // Sink: first leaf in the forward expansion (not in the backward BFS context).
                std::vector<DependencyVertex> updateSinkDVs = findSinkDVs(writeLocalVtx, &writeVtx[writeSrc]);
                for (auto &perSite : splitBySite(writeLocalVtx, writeSrc)) {
                    for (const auto &sinkDV : updateSinkDVs) {
                        auto writeVertices = filter_vertices_for_sink(perSite, sinkDV, updateSinkDVs);
                        chains.push_back(SOChain(depGraphs[index].get(), esg,
                                                 so, g[so].name, g[so].esgId.second,
                                                 std::move(writeVertices),
                                                 std::unordered_set<vertex_t>(), true, chainId++,
                                                 sinkDV));
                    }
                }

            // 2) Otherwise, create a multi-execution path: pure read x write paths
            } else {
                if (!readSitesCache.count(readDst)) {
                    std::unordered_set<vertex_t> readVtx;
                    std::deque<vertex_t> work;
                    // 1. Non-SO backward BFS from readDst to collect its context.
                    work.push_back(readDst);
                    while (!work.empty()) {
                        auto v = work.front(); work.pop_front();
                        for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                            auto src = boost::source(*ei, g);
                            if (g[*ei].label != "read_from"_cs && !g[src].isSO && !readVtx.count(src)) {
                                readVtx.insert(src);
                                work.push_back(src);
                            }
                        }
                    }

                    // 2. Forward BFS from readDst: collect the full read side, but skip
                    //    update-read destinations so the update body stays on write side only.
                    readVtx.insert(readDst);
                    work.push_back(readDst);
                    while (!work.empty()) {
                        auto v = work.front(); work.pop_front();
                        for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
                            auto tgt = boost::target(*ei, g);
                            if (!readVtx.count(tgt)) { readVtx.insert(tgt); work.push_back(tgt); }
                        }
                    }
                    readSitesCache[readDst] = splitReadBySite(readVtx, readDst);

                    // Clean up unreachable RETURNs in read path.
                    for (auto &perSiteR : readSitesCache[readDst]) {
                        keep_one_return_connected(g, perSiteR);
                    }
                }
                const auto &readSites = readSitesCache[readDst];

                // 3. Emit one chain per (write call site × read call site) combination.
                for (auto src : sources) {
                    for (auto &perSiteW : splitBySite(writeVtx[src], src)) {
                        for (const auto &perSiteR : readSites) {
                            // Clean up unreachable RETURNs in write path.
                            keep_one_return_connected(g, perSiteW);

                            std::vector<DependencyVertex> sinkDVs = findSinkDVs(perSiteR);
                            for (const auto &sinkDV : sinkDVs) {
                                auto writeVertices = filter_vertices_for_sink(perSiteW, sinkDV, sinkDVs);
                                auto readVertices = filter_vertices_for_sink(perSiteR, sinkDV, sinkDVs);
                                chains.push_back(SOChain(depGraphs[index].get(), esg,
                                                         so, g[so].name, g[so].esgId.second,
                                                         std::move(writeVertices),
                                                         std::move(readVertices),
                                                         false, chainId++, sinkDV));
                            }
                        }
                    }
                }
            }
        }
    }

    return chains;
}

std::vector<std::pair<DependencyGraphs::vertex_t, DependencyGraphs::vertex_t>>
DependencyGraphs::add_chain_satellites(size_t index,
                                       const std::vector<SOChain> &readChains,
                                       const std::vector<SOChain> &writeChains) {
    auto &g = *depGraphs[index];
    std::vector<std::pair<vertex_t, vertex_t>> rankPairs;

    // Helper: add a satellite circle node next to vertex v.
    auto addSat = [&](vertex_t v, const cstring &col, size_t chainId) -> vertex_t {
        vertex_t sat = boost::add_vertex(g);
        g[sat].name = cstring::to_cstring(chainId);
        g[sat].shape = "circle"_cs;
        g[sat].color = col;
        g[sat].isSatellite = true;
        auto [e, ok] = boost::add_edge(v, sat, g);
        if (ok) {
            g[e].label = ""_cs;
            g[e].style = "dashed"_cs;
            g[e].color = "grey"_cs;
            g[e].type = DepEdgeType::SATELITE;
        }
        rankPairs.emplace_back(v, sat);
        return sat;
    };

    // Chain 1: nowrite reads
    for (const auto &chain : readChains) {
        std::map<vertex_t, vertex_t> sateliteVertices;
        sateliteVertices.insert({chain.soVertex, addSat(chain.soVertex, "green"_cs, chain.id + 1)});
        for (auto v : chain.readVertices)
            sateliteVertices.insert({v, addSat(v, "green"_cs, chain.id + 1)});

        std::vector<std::pair<vertex_t, vertex_t>> chainEdges;
        for (auto [ei, ee] = boost::edges(g); ei != ee; ++ei) {
            if (g[*ei].type != DepEdgeType::DEPENDS_ON) continue;
            auto src = boost::source(*ei, g);
            auto tgt = boost::target(*ei, g);
            if (sateliteVertices.count(src) && sateliteVertices.count(tgt))
                chainEdges.emplace_back(sateliteVertices[src], sateliteVertices[tgt]);
        }
        for (auto [src, tgt] : chainEdges) {
            auto [e, ok] = boost::add_edge(src, tgt, g);
            if (ok) {
                g[e].label = ""_cs;
                g[e].style = ""_cs;
                g[e].color = ""_cs;
                g[e].type = DepEdgeType::CHAIN_PATH;
            }
        }
    }

    // Chains 2+: per write-SO chains
    for (const auto &chain : writeChains) {
        auto sateliteId = chain.id + 1; // start from 1 since 0 is for nowrite chains
        std::map<vertex_t, vertex_t> writeSats;  // for satelite edges
        std::map<vertex_t, vertex_t> readSats;   // for satelite edges
        for (auto v : chain.writeVertices)
            writeSats[v] = addSat(v, "tomato"_cs, sateliteId);

        readSats[chain.soVertex] = addSat(chain.soVertex,
            chain.readVertices.size() ? "tomato:gold"_cs : "tomato"_cs, sateliteId);
        for (auto v : chain.readVertices)
            if (v != chain.soVertex) readSats[v] = addSat(v, "gold"_cs, sateliteId);

        // Collect edges to mirror before modifying the graph — adding edges while
        // iterating boost::edges() invalidates vecS iterators.
        std::vector<std::pair<vertex_t, vertex_t>> chainEdges;
        for (auto [ei, ee] = boost::edges(g); ei != ee; ++ei) {
            if (g[*ei].type != DepEdgeType::DEPENDS_ON) continue;
            auto src = boost::source(*ei, g);
            auto tgt = boost::target(*ei, g);
            if (writeSats.count(src) && writeSats.count(tgt))
                chainEdges.emplace_back(writeSats[src], writeSats[tgt]);
            if (readSats.count(src) && readSats.count(tgt))
                chainEdges.emplace_back(readSats[src], readSats[tgt]);
            // Add writeSats -> soVertex
            if (writeSats.count(src) && tgt == chain.soVertex)
                chainEdges.emplace_back(writeSats[src], readSats[chain.soVertex]);
        }
        for (auto [src, tgt] : chainEdges) {
            auto [e, ok] = boost::add_edge(src, tgt, g);
            if (ok) {
                g[e].label = ""_cs;
                g[e].style = ""_cs;
                g[e].color = ""_cs;
                g[e].type = DepEdgeType::CHAIN_PATH;
            }
        }
    }

    return rankPairs;
}

void DependencyGraphs::inject_rank_groups(
        const std::filesystem::path &filepath,
        const std::vector<std::pair<vertex_t, vertex_t>> &pairs) {
    if (pairs.empty()) return;

    std::ifstream in(filepath.string());
    if (!in.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    in.close();

    // Find last closing brace and insert rank directives before it.
    int insertPos = -1;
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
        if (lines[i].find('}') != std::string::npos) { insertPos = i; break; }
    }
    if (insertPos == -1) return;

    for (const auto &[orig, sat] : pairs) {
        lines.insert(lines.begin() + insertPos,
            "    {rank=same; " + std::to_string(orig) + "; " + std::to_string(sat) + ";}");
        ++insertPos;  // keep subsequent insertions after already-inserted lines
    }

    std::ofstream out(filepath.string());
    for (const auto &l : lines) out << l << "\n";
}

}  // namespace P4::P4StateDependency
