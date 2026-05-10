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
    eData.type = DepEdgeType::DEPENDS_ON;

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

std::unordered_set<DependencyGraphs::vertex_t>
DependencyGraphs::get_nowrite_so_vertices(size_t index, size_t *leafCount) const {
    const auto &g = *depGraphs[index];
    std::unordered_set<vertex_t> result;
    if (boost::num_vertices(g) == 0) return result;

    // Collect SO vertices with no incoming "write_to" edge.
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
    if (noWriteSOs.empty()) return result;

    // Forward BFS from those SOs — collect every reachable vertex.
    std::deque<vertex_t> work(noWriteSOs.begin(), noWriteSOs.end());
    for (auto v : noWriteSOs) result.insert(v);
    while (!work.empty()) {
        auto v = work.front(); work.pop_front();
        for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
            auto tgt = boost::target(*ei, g);
            if (!result.count(tgt)) { result.insert(tgt); work.push_back(tgt); }
        }
    }

    if (leafCount) {
        *leafCount = 0;
        for (auto lv : leaves[index])
            if (result.count(lv)) ++(*leafCount);
    }
    return result;
}

std::vector<DependencyGraphs::SOChain>
DependencyGraphs::get_data_write_so_chains(size_t index,
                                          std::unordered_set<cstring> *soNames,
                                          size_t *writeSourceCount) const {
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
    if (writeSourceCount) *writeSourceCount = writeSources.size();

    // Collect distinct read-destination vertices from writeSO
    std::map<vertex_t, std::unordered_set<vertex_t>> readDestinations;
    for (auto [so, _] : writeSources) {
        for (auto [ei, ee] = boost::out_edges(so, g); ei != ee; ++ei) {
            if (g[*ei].label == "read_from"_cs) {
                auto tgt = boost::target(*ei, g);
                readDestinations[so].insert(tgt);
            }
        }
    }

    // Get SO chain for each (writeSO, so, readDst) pair
    for (auto [so, sources] : writeSources) {
        // Different readDst can lead to different chains
        for (auto readDst : readDestinations[so]) {
            BUG_CHECK(!sources.count(readDst),
                    "Read destination vertices should not overlap with write source vertices");
            std::unordered_set<vertex_t> readVtx;
            std::deque<vertex_t> work;
            // 1. Non-SO backward BFS from each read vertex until hitting root
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

            // 2. add SO and everything forward-reachable from it to readVtx.
            readVtx.insert(so);
            work.push_back(so);
            while (!work.empty()) {
                auto v = work.front(); work.pop_front();
                for (auto [ei, ee] = boost::out_edges(v, g); ei != ee; ++ei) {
                    auto tgt = boost::target(*ei, g);
                    if (!readVtx.count(tgt)) { readVtx.insert(tgt); work.push_back(tgt); }
                }
            }

            // 3. For each write source, backward BFS until hitting roots. Collect visited vertices as writeVtx.
            for (auto src : sources) {
                BUG_CHECK(readVtx.size() > 0 && !readVtx.count(src),
                        "non-empty readVertices should not contain write source vertices");

                // Write side: {so} + backward BFS until hitting roots
                std::unordered_set<vertex_t> writeVtx;
                std::deque<vertex_t> bwWork;
                writeVtx.insert(src);
                bwWork.push_back(src);
                bool isSinglePath = true;
                while (!bwWork.empty()) {
                    auto v = bwWork.front(); bwWork.pop_front();
                    for (auto [ei, ee] = boost::in_edges(v, g); ei != ee; ++ei) {
                        auto tgt = boost::source(*ei, g);
                        if (!writeVtx.count(tgt)) {
                            writeVtx.insert(tgt);
                            bwWork.push_back(tgt);
                            if (readVtx.count(tgt)) {
                                // Two-path detected: one path from src to so via tgt,
                                // another path from src to so via readVtx.
                                isSinglePath = false;
                            }
                        }
                    }
                }
                chains.push_back({so, g[so].name, g[so].esgId.second,
                                std::move(writeVtx), readVtx, isSinglePath});
            }
        }
    }

    return chains;
}

std::vector<std::pair<DependencyGraphs::vertex_t, DependencyGraphs::vertex_t>>
DependencyGraphs::add_chain_satellites(size_t index) {
    auto &g = *depGraphs[index];
    std::vector<std::pair<vertex_t, vertex_t>> rankPairs;

    int chainId = 1;

    // Helper: add a satellite circle node next to vertex v.
    auto addSat = [&](vertex_t v, const cstring &col, int chainId) -> vertex_t {
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
    auto nowriteSet = get_nowrite_so_vertices(index);
    if (!nowriteSet.empty()) {
        // TODO: check node color
        for (auto v : nowriteSet)
            addSat(v, "lightyellow"_cs, chainId);
        ++chainId;
    }

    // Chains 2+: per write-SO chains
    for (const auto &chain : get_data_write_so_chains(index, nullptr)) {
        std::map<vertex_t, vertex_t> writeSats;  // for satelite edges
        std::map<vertex_t, vertex_t> readSats;   // for satelite edges
        for (auto v : chain.writeVertices)
            writeSats[v] = addSat(v, "tomato"_cs, chainId);

        readSats[chain.soVertex] = addSat(chain.soVertex,
            chain.isSinglePath ? "tomato"_cs : "tomato:gold"_cs, chainId);
        for (auto v : chain.readVertices)
            if (v != chain.soVertex) {
                if (chain.isSinglePath)
                    readSats[v] = addSat(v, "tomato"_cs, chainId);
                else
                    readSats[v] = addSat(v, "gold"_cs, chainId);
            }

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
                g[e].label = cstring::to_cstring(chainId);
                g[e].style = ""_cs;
                g[e].color = ""_cs;
                g[e].type = DepEdgeType::CHAIN_PATH;
            }
        }
        ++chainId;
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
