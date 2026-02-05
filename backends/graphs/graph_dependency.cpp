/**
 * @author Jiwon Kim
 */

#include "graph_dependency.h"
#include "frontends/p4/methodInstance.h"

namespace P4::graphs {
const Graphs::varset_t Graphs::emptyVarSet;

bool GraphDependency::is_stateful(const IR::Node *node) {
    if (node->is<IR::BaseAssignmentStatement>()) {
        auto stmt = node->to<IR::BaseAssignmentStatement>();
        // Check right expression
        auto rs = stmt->right;
        if (rs->is<IR::MethodCallExpression>()) {
            auto rmce = rs->to<IR::MethodCallExpression>();
            auto inst = P4::MethodInstance::resolve(rmce, refMap, typeMap);

            if (inst->is<P4::ExternMethod>()) {
                auto em = inst->to<P4::ExternMethod>();
                std::string statefulExternNames[4] = {"Counter", "Meter", "Register", "RegisterAction"};
                for (const std::string &name : statefulExternNames) {
                    if (em->originalExternType->getName().name == name) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

std::vector<Graphs::vertex_t> GraphDependency::get_vertices_per_type(Graph *g, VertexType type, bool isStateful) {
    auto vertices = boost::vertices(*g);
    std::vector<Graphs::vertex_t> found_vertices;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (vinfo.isStateful == isStateful) {
            if (type == VertexType::EMPTY || vinfo.type == type) {
                found_vertices.push_back(*vit);
            }
        } else if (isStateful) {
            /* Additionally check nodes of vertex */
            for (auto node : vinfo.nodes) {
                if (is_stateful(node)) {
                    found_vertices.push_back(*vit);
                    break;
                }
            }
        }
    }
    return found_vertices;
}

// find path from tv to sv
std::vector<Graphs::vertex_t> GraphDependency::find_path_from_vertices(Graph *g,
        Graphs::vertex_t &sv, Graphs::vertex_t &dv) {
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

    visited[index[dv]] = true;
    q.push(dv);
    bool found = false;
    while (!q.empty() && !found) {
        Graphs::vertex_t u = q.front();
        q.pop();

        auto [ei, ei_end] = boost::in_edges(u, *g);
        for (; ei != ei_end; ++ei) {
            auto edge = (*g)[*ei];
            if (edge.type != EdgeType::CONTROL)
                continue;
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

void GraphDependency::dump_def_use() {
    std::vector<std::string> defs;
    std::vector<std::string> uses;

    std::stringstream defuse_ss;
    defuse_ss << *defUse;

    std::vector<std::string> *target = &defs;
    std::string line;
    while (std::getline(defuse_ss, line)) {
        if (line == "defs:") {
            target = &defs;
        } else if (line == "uses:") {
            target = &uses;
        } else {
            target->push_back(line);
        }
    }

    // Debug output
    std::cout << "defs:" << std::endl;
    for (auto &line : defs) std::cout << line << std::endl;
    std::cout << "uses:" << std::endl;
    for (auto &line : uses) std::cout << line << std::endl;
}

void GraphDependency::process() {
    for (auto g : controlGraphsArray) {
        process_subgraph(g);
        //dump_vars_in_graph(g);
    }
}

void GraphDependency::dfs_all_paths(Graph *g, Graphs::vertex_t &cur, Graphs::vertex_t &dst,
        std::vector<Graphs::vertex_t> &path,
        std::vector<std::vector<Graphs::vertex_t>> &allPaths) {

    path.push_back(cur);

    if (cur == dst) {
        allPaths.push_back(path);
    } else {
        auto [ei, ei_end] = boost::out_edges(cur, *g);
        for (; ei != ei_end; ++ei) {
            auto edge = (*g)[*ei];
            if (edge.type != EdgeType::CONTROL)
                continue;
            auto next = boost::target(*ei, *g);
            dfs_all_paths(g, next, dst, path, allPaths);
        }
    }

    path.pop_back();
}

std::optional<Graphs::vertex_t> GraphDependency::get_defuse_action(Graph *g, Graphs::vertex_t &vit,
        std::vector<Graphs::vertex_t> &pathToDst) {
    Graphs::vertex_t v = vit;
    while (true) {
        // 1. find all uses and next child
        std::vector<Graphs::vertex_t> uses;
        auto [ei, ei_end] = boost::out_edges(v, *g);
        int numChild = 0;
        for (; ei != ei_end; ++ei) {
            auto u = boost::target(*ei, *g);
            auto edge = (*g)[*ei];
            if (edge.type == EdgeType::CONTROL) {
                numChild ++;
                if (numChild == 1) {
                    pathToDst.push_back(v);
                    v = u;
                }
            } else if (edge.type == EdgeType::DEFUSE) {
                uses.push_back(u);
            }
        }
        // 2. Finish if not one child
        if (numChild != 1)
            return {};

        // 3. check if one of uses is action
        for (auto u : uses) {
            auto uInfo = (*g)[u];
            if (uInfo.type == VertexType::ACTION)
                return u;
        }
    }
    return {};
}

bool GraphDependency::add_on_miss_defuse_path(Graph *g, Graphs::vertex_t &vit,
        std::vector<Graphs::vertex_t> &postPaths) {
    // 1. add vertices after add-on-miss table until it finds action
    std::queue<Graphs::vertex_t> q;
    q.push(vit);
    std::vector<Graphs::vertex_t> actions;
    while (!q.empty()) {
        Graphs::vertex_t u = q.front();
        q.pop();

        auto [ei, ei_end] = boost::out_edges(u, *g);
        for (; ei != ei_end; ++ei) {
            auto edge = (*g)[*ei];
            if (edge.type != EdgeType::CONTROL)
                continue;
            auto v = boost::target(*ei, *g);
            auto vinfo = (*g)[v];
            if (vinfo.type == VertexType::ACTION) {
                actions.push_back(v);
            } else {
                postPaths.push_back(v);
                q.push(v);
            }
        }
    }

    // 2. find miss-to-hit defuse
    for (auto sit : actions) {
        std::vector<Graphs::vertex_t> pathToDst;
        auto dit = get_defuse_action(g, sit, pathToDst);
        if (dit.has_value()) {
            postPaths.insert(postPaths.end(), pathToDst.begin(), pathToDst.end());
            postPaths.push_back(dit.value());
            return true;
        }
        pathToDst.clear();
    }
    postPaths.clear();
    return false;
}

int GraphDependency::find_all_paths(Graph *g,
        Graphs::vertex_t &sv, Graphs::vertex_t &dv) {
    std::vector<Graphs::vertex_t> pathMd;
    std::vector<std::vector<Graphs::vertex_t>> allPaths;

    dfs_all_paths(g, sv, dv, pathMd, allPaths);

    if (allPaths.size() == 0) {
        LOG4("No paths found");
        return false;
    }

    auto src = (*g)[sv];
    auto dst = (*g)[dv];

    std::vector<Graphs::vertex_t> postPaths;
    if (dst.type == VertexType::TABLE) {
        // add-on-miss
        add_on_miss_defuse_path(g, dv, postPaths);
    }

    int numPath = 0;
    for (auto path : allPaths) {
        std::cout << ++numPath << ":";
        path.insert(path.end(), postPaths.begin(), postPaths.end());
        for (auto v : path) {
            auto vinfo = (*g)[v];
            std::cout << "-> " << vinfo.name;
        }
        std::cout << std::endl;
    }

    return allPaths.size();
}

void GraphDependency::analyze_subgraph(Graph *g) {
    // 1. Get table / stateful vertices
    auto tableVertices = get_vertices_per_type(g, VertexType::TABLE, false);
    auto statefulVertices = get_vertices_per_type(g, VertexType::EMPTY, true);

    if (tableVertices.size() == 0 || statefulVertices.size() == 0)
        return;

    // 2. While tracking topological order, check table-to-stateful case
    std::vector<Graphs::vertex_t> order;
    boost::topological_sort(*g, std::back_inserter(order));
    int numCases = 0;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        // Find tableVertex
        if (std::find(tableVertices.begin(), tableVertices.end(), *it)
                == tableVertices.end())
            continue;

        for (auto jt = it + 1; jt != order.rend(); ++jt) {
            // Find statefulVertex
            if (std::find(statefulVertices.begin(), statefulVertices.end(), *jt)
                    == statefulVertices.end())
                continue;

            auto tv = (*g)[*it];
            auto sv = (*g)[*jt];
            auto numPaths = find_all_paths(g, *it, *jt);

            if (numPaths > 0)
                numCases ++;
            std::cout << tv.name << "->" << sv.name <<
                " (" << numPaths << ")" << std::endl;
        }
    }
    std::cout << std::endl;

    std::cout << "#Stateful: " << statefulVertices.size()
        << ", #Table: " << tableVertices.size()
        << ", #Cases: " << numCases << std::endl;
}

void GraphDependency::analyze() {
    for (auto g : controlGraphsArray) {
        analyze_subgraph(g);
    }
}

std::optional<Graphs::vertex_t> GraphDependency::add_var_in_cfg(Graph *g, const ComputeDefUse::loc_t *loc, bool isDef) {
    auto *l = loc;
    while (l != nullptr) {
        auto *v = l->node;
        auto vit = find_node_by_ptr(g, v);
        if (vit.has_value()) {
            auto &vinfo = (*g)[vit.value()];
            if (isDef) {
                vinfo.defs[v].insert(loc->node);
            } else {
                vinfo.uses[v].insert(loc->node);
            }
            return vit.value();
        }
        l = l->parent;
    }
    return {};
}

cstring GraphDependency::join_var_names(const varset_t &vars, bool hasId) {
    std::stringstream sstream;
    bool first = true;
    for (auto *v : vars) {
        if (!first) sstream << ", ";
        first = false;
        v->dbprint(sstream);
        if (hasId) sstream << '<' << v->id << '>';
    }
    return cstring(sstream);
}

void GraphDependency::split_cfg_vertex(Graph *g, const Graphs::vertex_t &v,
        hvec_map<const IR::Node *, const ComputeDefUse::loc_t *> &nodeToVarMap) {
    auto &vinfo = (*g)[v];
    if (vinfo.nodes.size() <= 1)
        return;

    std::vector<const IR::Node *> curNodes;
    bool hasVar = false;
    bool updateLast = false;
    for (auto node : vinfo.nodes) {
        bool isVarNode = nodeToVarMap.find(node) != nodeToVarMap.end();
        if (!isVarNode) {
            curNodes.push_back(node);

        } else if (!hasVar) {
            curNodes.push_back(node);
            hasVar = true;

        } else {
            auto u = add_vertex_nodes(g, get_vertex_name(curNodes), vinfo.type, vinfo.isStateful, curNodes);
            std::vector<Graphs::edge_t> to_remove;
            auto [ei, ei_end] = boost::in_edges(v, *g);
            // move parent's in edges to u
            for (; ei != ei_end; ++ei) {
                auto p = boost::source(*ei, *g);
                auto ep = (*g)[*ei];
                add_edge(g, p, u, ep.name, ep.type);
                to_remove.push_back(*ei);
            }
            for (auto &e : to_remove) remove_edge(e, *g);
            add_edge(g, u, v, cstring::empty, EdgeType::CONTROL);

            curNodes.clear();
            curNodes.push_back(node);
            hasVar = true;
            updateLast = true;
        }
    }

    if (updateLast) {
        vinfo.name = get_vertex_name(curNodes);
        vinfo.nodes.clear();
        vinfo.nodes.insert(vinfo.nodes.end(), curNodes.begin(), curNodes.end());
    }
}

std::optional<const IR::Node *> GraphDependency::find_node_by_loc(Graph *g, const ComputeDefUse::loc_t *loc) {
    auto *l = loc;
    while (l != nullptr) {
        auto *v = l->node;
        auto vit = find_node_by_ptr(g, v);
        if (vit.has_value())
            return v;
        l = l->parent;
    }
    return {};
}

void GraphDependency::process_subgraph(Graph *g) {
    // 1. Split cfg graphs
    auto defuse = defUse->getAllDefUse();
    ComputeDefUse::locset_t locset;
    for (auto &p : defuse.uses)
        for (auto *loc : p.second)
            locset.insert(loc);
    for (auto &p : defuse.defs)
        for (auto *loc : p.second)
            locset.insert(loc);

    hvec_map<const IR::Node *, const ComputeDefUse::loc_t *> nodeToVarMap;
    for (auto *loc : locset) {
        auto v = find_node_by_loc(g, loc);
        if (!v.has_value())
            continue;
        nodeToVarMap[v.value()] = loc;
    }

    // split should be called before connecting DDG edges
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit)
        split_cfg_vertex(g, *vit, nodeToVarMap);

    // 2. Map def-use variables to CFG vertices by using loc_t
    hvec_map<const IR::Node *, Graphs::vertex_t> defToVertexMap;
    hvec_map<const IR::Node *, Graphs::vertex_t> useToVertexMap;
    hvec_map<Graphs::vertex_t, varset_t> defMap;
    hvec_map<Graphs::vertex_t, varset_t> useMap;

    // 2-1) collect all uses (used variables)
    for (auto &p : defuse.uses) {
        for (auto *loc : p.second) {
            auto vit = add_var_in_cfg(g, loc, false);
            if (vit.has_value()) {
                useMap[vit.value()].insert(loc->node);
                useToVertexMap[loc->node] = vit.value();
            }
        }
    }

    // 2-2) collect all defs (defined variables)
    for (auto &p : defuse.defs) {
        for (auto *loc : p.second) {
            auto vit = add_var_in_cfg(g, loc, true);
            if (vit.has_value()) {
                defMap[vit.value()].insert(loc->node);
                defToVertexMap[loc->node] = vit.value();
            }
        }
    }

    // 2-3) collect defs in special node (e.g., START, EXIT)
    vertices = boost::vertices(*g);
    std::optional<Graphs::vertex_t> startVit, exitVit;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        // Covered by defuse
        if (vinfo.nodes.size() > 0)
            continue;

        // __START__
        auto defIt = vinfo.defs.find(nullptr);
        if (defIt != vinfo.defs.end()) {
            startVit = *vit;
            auto defSet = defIt->second;
            for (const auto *def : defSet) {
                defMap[*vit].insert(def);
                defToVertexMap[def]= *vit;
            }
        }
        // __EXIT__
        auto useIt = vinfo.uses.find(nullptr);
        if (useIt != vinfo.uses.end()) {
            exitVit = *vit;
            auto useSet = useIt->second;
            for (const auto *use : useSet) {
                useMap[*vit].insert(use);
                useToVertexMap[use]= *vit;
            }
        }
    }

    // 3. Collect Def-Use edges
    vertices = boost::vertices(*g);
    hvec_map<std::pair<Graphs::vertex_t, Graphs::vertex_t>, varset_t> defUseEdgeMap;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        // 3-1) From defined vars (src), get all usages (sink)
        for (auto v : defMap[*vit]) {
            for (const auto *sinkLoc : defUse->getUses(v)) {
                // Find use defined by v
                auto sinkVit = useToVertexMap.find(sinkLoc->node);
                if (sinkVit == useToVertexMap.end())
                    continue;
                auto sink = sinkVit->second;
                defUseEdgeMap[{*vit, sink}].insert(v);
            }
        }
        // 3-2) From used vars (sink), get all definitions (src)
        for (auto v : useMap[*vit]) {
            for (const auto *srcLoc : defUse->getDefs(v)) {
                // Find def used by v
                auto srcVit = defToVertexMap.find(srcLoc->node);
                if (srcVit == defToVertexMap.end())
                    continue;
                auto src = srcVit->second;
                defUseEdgeMap[{src, *vit}].insert(v);
            }
        }
    }

    // 4. Draw DDG edges
    for (auto &p : defUseEdgeMap) {
        auto src = p.first.first;
        auto sink = p.first.second;
        // Skip inout parameters
        if (src == startVit && sink == exitVit)
            continue;

        auto &edgeVars = p.second;
        BUG_CHECK(src != sink, "src and sink should be different");
        add_edge(g, src, sink, join_var_names(edgeVars, false), EdgeType::DEFUSE);
    }

    // 5. clear
    defUseEdgeMap.clear();
    vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        defMap[*vit].clear();
        useMap[*vit].clear();
    }
}

void GraphDependency::dump_vars_in_graph(Graph *g) {
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        // START
        if (vinfo.nodes.size() == 0) {
            auto defIt = vinfo.defs.find(nullptr);
            if (defIt != vinfo.defs.end()) {
                auto defSet = defIt->second;
                std::cout << vinfo.name << "(" << defSet.size() << "): "
                          << join_var_names(defSet, false) << std::endl;
            }
        } else {
            std::cout << vinfo.name << std::endl; // print name
            // Normal IR nodes
            for (const auto *n : vinfo.nodes) {
                auto defIt = vinfo.defs.find(n);
                const varset_t defSet = defIt == vinfo.defs.end() ?
                                        emptyVarSet : defIt->second;
                auto useIt = vinfo.uses.find(n);
                const varset_t useSet = useIt == vinfo.uses.end() ?
                                        emptyVarSet : useIt->second;

                std::stringstream sstream;
                n->dbprint(sstream);

                std::cout << "  " << cstring(sstream) << std::endl
                          << "  - def(" << defSet.size() << "): "
                          << join_var_names(defSet, true) << std::endl
                          << "  - use(" << useSet.size() << "): "
                          << join_var_names(useSet, true) << std::endl;
            }
        }
        std::cout << std::endl;
    }
}

}  // namespace P4::graphs
