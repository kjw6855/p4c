/**
 * @author Jiwon Kim
 */

#include "graph_dependency.h"
#include "frontends/p4/methodInstance.h"

namespace P4::graphs {
const Graphs::varset_t Graphs::emptyVarSet;

bool GraphDependency::is_stateful(const Graphs::NodeId &nid) {
    if (nid.node->is<IR::BaseAssignmentStatement>()) {
        auto stmt = nid.node->to<IR::BaseAssignmentStatement>();
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
            for (auto &nid : vinfo.nodes) {
                if (is_stateful(nid)) {
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
            auto edge = g->root()[*ei];
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

int GraphDependency::find_all_paths(Graph *g, Graphs::vertex_t &sv, Graphs::vertex_t &dv,
                                    std::vector<std::vector<Graphs::vertex_t>> &allPaths) {
    std::vector<Graphs::vertex_t> pathMd;

    dfs_all_paths(g, sv, dv, pathMd, allPaths);

    if (allPaths.size() == 0) {
        LOG4("No paths found");
        return 0;
    }

    auto src = (*g)[sv];
    auto dst = (*g)[dv];

    std::vector<Graphs::vertex_t> postPaths;
    if (dst.type == VertexType::TABLE && dst.isStateful) {
        // add-on-miss
        add_on_miss_defuse_path(g, dv, postPaths);
    }

    int numPath = 0;
    for (auto &path : allPaths) {
        std::cout << ++numPath << ":";
        path.insert(path.end(), postPaths.begin(), postPaths.end());
        for (auto v : path) {
            auto vinfo = (*g)[v];
            std::cout << "-> " << vinfo.name;
        }
        std::cout << std::endl;
    }

    return numPath;
}

bool GraphDependency::is_empty_action(Graph *g, Graphs::vertex_t u) {
    auto uinfo = (*g)[u];
    if (uinfo.type != VertexType::ACTION) return false;

    auto [ei, ei_end] = boost::out_edges(u, *g);
    for (; ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::CONTROL)
            continue;
        Graphs::vertex_t v = boost::target(*ei, *g);
        auto vinfo = (*g)[v];
        // True if found OTHER (__EMPTY__) after ACTION
        if (vinfo.type == VertexType::OTHER) {
            return true;
        }
    }

    return false;
}

void GraphDependency::find_action_vertices(Graph *g, Graphs::vertex_t u,
                          hvec_map<Graphs::vertex_t, cstring> &foundVertices,
                          cstring actionName) {
    auto [ei, ei_end] = boost::out_edges(u, *g);
    for (; ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::CONTROL)
            continue;
        Graphs::vertex_t v = boost::target(*ei, *g);
        auto vinfo = (*g)[v];
        if (actionName.size() == 0) {
            find_action_vertices(g, v, foundVertices,
                                 vinfo.type == VertexType::ACTION ?
                                 vinfo.name : ""_cs);
        } else if (vinfo.type != VertexType::OTHER) {
            // Find if action has body
            foundVertices[v] = actionName;
        }
    }
}

std::vector<Graphs::vertex_t> GraphDependency::find_all_action_vertices(Graph *g) {
    auto vertices = boost::vertices(*g);
    std::vector<Graphs::vertex_t> foundVertices;

    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (vinfo.type != VertexType::ACTION)
            continue;

        // ACTION has been found
        auto [ei, ei_end] = boost::out_edges(*vit, *g);
        for (; ei != ei_end; ++ei) {
            auto edge = (*g)[*ei];
            if (edge.type != EdgeType::CONTROL)
                continue;

            Graphs::vertex_t u = boost::target(*ei, *g);
            auto uinfo = (*g)[u];
            if (uinfo.type != VertexType::OTHER)
                foundVertices.push_back(u);
        }
    }

    return foundVertices;
}

std::optional<Graphs::vertex_t> GraphDependency::get_match_vertex(Graph *g, Graphs::vertex_t src) {
    auto [ei, ei_end] = boost::out_edges(src, *g);
    for (; ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::CONTROL)
            continue;
        Graphs::vertex_t v = boost::target(*ei, *g);
        auto vinfo = (*g)[v];
        if (vinfo.type == VertexType::KEY)
            return v;
    }
    return {};
}

bool GraphDependency::has_non_exact_match(const IR::Node *node) {
    if (!node->is<IR::Key>()) return false;

    auto key = node->to<IR::Key>();
    for (auto elVec : key->keyElements) {
        if (elVec->matchType->path->name.name != "exact"_cs)
            return true;
    }

    return false;
}

bool GraphDependency::has_non_exact_match(Graph *g, Graphs::vertex_t v) {
    auto mv = get_match_vertex(g, v);
    auto vinfo = (*g)[v];
    bool hasNonExactMatch = false;
    if (mv.has_value()) {
        auto mvInfo = (*g)[mv.value()];
        BUG_CHECK(mvInfo.nodes.size() == 1, "There should be one node for Key");
        hasNonExactMatch = has_non_exact_match(mvInfo.nodes[0].node);
        auto logstr = (hasNonExactMatch ? "Has"_cs : "No"_cs) + " non-exact match: "_cs;
        LOG5(logstr << vinfo.name);
    }

    return hasNonExactMatch;
}

GraphDependency::VariableType GraphDependency::get_variable_type(const IR::Node *node, const IR::Node *var) {
    if (node->is<IR::ActionListElement>()) {
        // Action param is DATA and match is index
        return VariableType::DATA;

    } else if (auto *mcs = node->to<IR::MethodCallStatement>()) {
        auto instance = P4::MethodInstance::resolve(mcs->methodCall, refMap, typeMap);

        if (auto *em = instance->to<P4::ExternMethod>()) {
            if (em->originalExternType->getName().name == "register") {
                // check arguments
                if (em->method->name.name == "read") {
                    const IR::Node *arg0 = em->expr->arguments->at(0)->expression;
                    const IR::Node *arg1 = em->expr->arguments->at(1)->expression;
                    if (arg0->srcInfo == var->srcInfo)
                        return VariableType::DATA;
                    else if (arg1->srcInfo == var->srcInfo)
                        return VariableType::INDEX;

                } else if (em->method->name.name == "write") {
                    const IR::Node *arg1 = em->expr->arguments->at(0)->expression;
                    if (arg1->srcInfo == var->srcInfo)
                        return VariableType::INDEX;
                }
            }
        }
    }

    return VariableType::NONE;
}

void GraphDependency::dfs_table_so_policy(Graph *g,
                                          Graphs::vertex_t u,
                                          std::vector<Graphs::vertex_t> &statefulVertices,
                                          varset_t &vars,
                                          DfsSecResult &dsr) {
    auto uinfo = (*g)[u];
    LOG5("   check " << uinfo.name);
    auto [ei, ei_end] = boost::out_edges(u, *g);
    for (; ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::DEFUSE)
            continue;

        // 1. check edge.vars in vars
        varset_t usedVars;
        for (auto ev : edge.vars) {
            if (vars.find(ev) != vars.end())
                usedVars.insert(ev);
        }
        if (usedVars.size() == 0)
            continue;

        // 2. Found if the vertex is one of stateful
        Graphs::vertex_t v = boost::target(*ei, *g);
        auto vinfo = (*g)[v];
        bool isStateful = std::find(statefulVertices.begin(), statefulVertices.end(), v)
                != statefulVertices.end();

        if (isStateful)
            LOG5("   FOUND: " << vinfo.name);

        // 3. collect new vars
        // TODO: optimize data structure
        // edge contains defs and uses, but currently it requires
        // defUse->getUses() again
        varset_t newVars;
        for (auto &nid : vinfo.nodes) {
            // 1) check if any used variables
            auto useIt = vinfo.uses.find(nid);
            if (useIt == vinfo.uses.end())
                continue;

            auto defIt = vinfo.defs.find(nid);
            bool hasNewVar = (defIt != vinfo.defs.end());

            bool found = false;
            for (auto uv : usedVars) {
                for (auto ul : defUse->getUses(uv)) {
                    // Search next variable for this node
                    if (useIt->second.find(ul->node) == useIt->second.end())
                        continue;

                    // If the node uses one of usedVars, store all defs
                    if (hasNewVar && !found) {
                        newVars.insert(defIt->second.begin(), defIt->second.end());
                        found = true;
                    }

                    // Find if the variable is used as index or data
                    if (isStateful) {
                        switch (get_variable_type(nid.node, ul->node)) {
                            case VariableType::INDEX:
                                dsr.indexVertices[*dsr.src].insert(v);
                                break;
                            case VariableType::DATA:
                                dsr.dataVertices[*dsr.src].insert(v);
                                break;
                            default:
                                break;
                        }
                    }
                }
            }
        }

        dfs_table_so_policy(g, v, statefulVertices, newVars, dsr);
    }
}

void GraphDependency::check_table_so_policy(Graph *g, Graphs::vertex_t src,
                                            std::vector<Graphs::vertex_t> &statefulVertices,
                                            DfsSecResult &dsr) {
    // I. find Action blocks
    //std::vector<std::pair<cstring, Graphs::vertex_t>> actStmtVertices;
    find_action_vertices(g, src, dsr.srcMap, ""_cs);

    std::cout << "actions:";
    for (auto &v : dsr.srcMap) std::cout << " " << v.first;
    std::cout << std::endl;

    for (auto &v : dsr.srcMap) {
        // 1. collect all new defs
        varset_t vars;
        auto vinfo = (*g)[v.first];
        for (auto &defs : vinfo.defs)
            vars.insert(defs.second.begin(), defs.second.end());

        // 2. run DFS
        dsr.src = &v.first;
        dfs_table_so_policy(g, v.first, statefulVertices, vars, dsr);
    }
}

void GraphDependency::analyze_subgraph(Graph *g) {
    // 1. Get table / stateful vertices
    auto tableVertices = get_vertices_per_type(g, VertexType::TABLE, false);
    auto statefulVertices = get_vertices_per_type(g, VertexType::EMPTY, true);

    if (tableVertices.size() == 0 || statefulVertices.size() == 0)
        return;

    // 2. replace add-on-miss table to hit action
    for (std::size_t i = 0; i < statefulVertices.size(); i++) {
        auto sit = statefulVertices[i];
        auto sInfo = (*g)[sit];
        if (sInfo.type == VertexType::TABLE && sInfo.isStateful) {
            std::vector<Graphs::vertex_t> postPaths;
            add_on_miss_defuse_path(g, sit, postPaths);
            if (postPaths.size() > 0) {
                statefulVertices[i] = postPaths.back();
            }
        }
    }

    std::cout << "#Stateful: " << statefulVertices.size()
        << ", #Table: " << tableVertices.size() << std::endl;

    for (auto v : tableVertices) {
        // 1) Run DFS to find all possible cases
        DfsSecResult dsr;
        check_table_so_policy(g, v, statefulVertices, dsr);

        // 2) Print path of each case to mitigate
        bool hasNonExactMatch = has_non_exact_match(g, v);
        for (auto &p : dsr.indexVertices) {
            if (hasNonExactMatch) {
                std::cout << "FOUND (B1/3): " << dsr.srcMap[p.first] << std::endl;
            } else {
                std::cout << "FOUND (B3): " << dsr.srcMap[p.first] << std::endl;
            }
            for (auto &iv : p.second) {
                auto ivinfo = (*g)[iv];
                std::cout << "--> " << ivinfo.name << std::endl;
            }
        }
        for (auto &p : dsr.dataVertices) {
            std::cout << "FOUND (B2): " << dsr.srcMap[p.first] << std::endl;
            for (auto &dv : p.second) {
                auto dvinfo = (*g)[dv];
                std::cout << "--> " << dvinfo.name << std::endl;
            }
        }
    }
}

std::size_t GraphDependency::dfs_find_cycle(Graph *g, Graphs::vertex_t u,
        Graphs::IndexMap &index,
        std::vector<bool> &visited, std::vector<bool> &recStack,
        std::vector<Graphs::vertex_t> &found_vertices,
        std::vector<cstring> &found_edges) {
    std::size_t uid = index[u];
    if (recStack[uid]) {
        found_vertices.push_back(u);
        std::cout << "Detected: " << uid << std::endl;
        return uid;
    }

    if (visited[uid]) return (std::size_t)-1;

    recStack[uid] = true;
    visited[uid] = true;
    found_vertices.push_back(u);

    auto [ei, ei_end] = boost::out_edges(u, *g);
    for (; ei != ei_end; ++ei) {
        Graphs::vertex_t v = boost::target(*ei, *g);

        auto edge = (*g)[*ei];
        found_edges.push_back(get_edge_type(edge.type) + ":"_cs + edge.name);
        auto detectedId = dfs_find_cycle(g, v, index, visited, recStack, found_vertices, found_edges);
        if (detectedId != (std::size_t)-1)
            return detectedId;
        found_edges.pop_back();
    }

    found_vertices.pop_back();
    recStack[uid] = false;
    return (std::size_t)-1;
}

bool GraphDependency::is_cyclic(Graph *g) {
    std::size_t n = num_vertices(*g);
    std::vector<bool> visited(n, false);
    std::vector<bool> recStack(n, false);
    std::vector<Graphs::vertex_t> found_vertices;
    std::vector<cstring> found_edges;

    auto index = boost::get(boost::vertex_index, *g);
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        if (visited[index[*vit]])
            continue;

        auto detectedId = dfs_find_cycle(g, *vit, index, visited, recStack,
                found_vertices, found_edges);

        if (detectedId != (std::size_t)-1) {
            // FOUND
            std::cout << "Cycle found:" << std::endl;
            bool doPrint = false;
            for (std::size_t i = 0; i < found_vertices.size(); i++) {
                auto c = found_vertices[i];
                if (!doPrint) {
                    if (index[c] != detectedId)
                        continue;
                    // Start printing
                    doPrint = true;
                } else if (i > 0) {
                    std::cout << "--" << found_edges[i - 1] << "-> ";
                }
                auto cinfo = (*g)[c];
                std::cout << cinfo.name << '(' << index[c] << ')' << std::endl;
            }
            std::cout << std::endl;
            return true;
        }
    }
    return false;
}

void GraphDependency::analyze() {
    for (auto g : controlGraphsArray) {
        BUG_CHECK (!is_cyclic(g), "Graph has cycle");
        analyze_subgraph(g);
    }
}

std::vector<Graphs::vertex_t> GraphDependency::add_var_in_cfg(Graph *g, const ComputeDefUse::loc_t *loc, bool isDef) {
    std::vector<Graphs::vertex_t> foundVertices;
    auto *l = loc;
    bool found = false;
    while (l != nullptr && !found) {
        auto *v = l->node;
        for (auto vit : find_node_by_ptr(g, v)) {
            // {vertex ID, Node ID}
            if (is_empty_action(g, vit.first)) {
                // FOUND but not used
                found = true;
                continue;
            }

            auto &vinfo = (*g)[vit.first];
            if (isDef) {
                vinfo.defs[vit.second].insert(loc->node);
            } else {
                vinfo.uses[vit.second].insert(loc->node);
            }
            found = true;
            foundVertices.push_back(vit.first);
        }
        l = l->parent;
    }
    return foundVertices;
}

void GraphDependency::split_cfg_vertex(Graph *g, const Graphs::vertex_t &v,
        hvec_map<const IR::Node *, ComputeDefUse::locset_t> &nodeToVarMap) {
    auto &vinfo = (*g)[v];
    if (vinfo.nodes.size() <= 1)
        return;

    std::vector<Graphs::NodeId> curNodes;
    bool hasVar = false;
    bool updateLast = false;
    for (auto &nid : vinfo.nodes) {
        bool isVarNode = nodeToVarMap.find(nid.node) != nodeToVarMap.end();
        if (!isVarNode) {
            curNodes.push_back(nid);

        } else if (!hasVar) {
            curNodes.push_back(nid);
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
            curNodes.push_back(nid);
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

std::vector<std::pair<Graphs::vertex_t, Graphs::NodeId>> GraphDependency::find_node_by_loc(Graph *g, const ComputeDefUse::loc_t *loc) {
    auto *l = loc;
    while (l != nullptr) {
        auto *v = l->node;
        auto nodes = find_node_by_ptr(g, v);
        if (nodes.size() > 0)
            return nodes;
        l = l->parent;
    }
    return {};
}

void GraphDependency::split_cfg_vertices(Graph *g) {
    auto defuse = defUse->getAllDefUse();
    ComputeDefUse::locset_t locset;
    for (auto &p : defuse.uses)
        for (auto *loc : p.second)
            locset.insert(loc);
    for (auto &p : defuse.defs)
        for (auto *loc : p.second)
            locset.insert(loc);

    hvec_map<const IR::Node *, ComputeDefUse::locset_t> nodeToVarMap;
    for (auto *loc : locset) {
        for (auto vit : find_node_by_loc(g, loc)) {
            // {vertex ID, Node ID}
            nodeToVarMap[vit.second.node].insert(loc);
        }
    }

    auto actionVertices = find_all_action_vertices(g);
    auto vertices = boost::vertices(*g);
    // split should be called before connecting DDG edges
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        // Skip if vertex is action statements
        if (std::find(actionVertices.begin(), actionVertices.end(), *vit)
                != actionVertices.end())
            continue;

        split_cfg_vertex(g, *vit, nodeToVarMap);
    }
}

void GraphDependency::process_subgraph(Graph *g) {
    // 1. Split cfg graphs
    auto defuse = defUse->getAllDefUse();
    if (splitVertex)
        split_cfg_vertices(g);

    // 2. Map def-use variables to CFG vertices by using loc_t
    hvec_map<const IR::Node *, Graphs::vertex_t> defToVertexMap;
    hvec_map<const IR::Node *, Graphs::vertex_t> useToVertexMap;
    hvec_map<Graphs::vertex_t, varset_t> defMap;
    hvec_map<Graphs::vertex_t, varset_t> useMap;

    // 2-1) collect all uses (used variables)
    for (auto &p : defuse.uses) {
        for (auto *loc : p.second) {
            for (auto vit : add_var_in_cfg(g, loc, false)) {
                useMap[vit].insert(loc->node);
                useToVertexMap[loc->node] = vit;
            }
        }
    }

    // 2-2) collect all defs (defined variables)
    for (auto &p : defuse.defs) {
        for (auto *loc : p.second) {
            for (auto vit : add_var_in_cfg(g, loc, true)) {
                defMap[vit].insert(loc->node);
                defToVertexMap[loc->node] = vit;
            }
        }
    }

    // 2-3) collect defs in special node (e.g., START, EXIT)
    auto vertices = boost::vertices(*g);
    std::optional<Graphs::vertex_t> startVit, exitVit;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        const auto &vinfo = (*g)[*vit];
        // Covered by defuse
        if (vinfo.nodes.size() > 0)
            continue;

        // __START__
        auto defIt = vinfo.defs.find(Graphs::globalNodeId);
        if (defIt != vinfo.defs.end()) {
            startVit = *vit;
            auto defSet = defIt->second;
            for (const auto *def : defSet) {
                defMap[*vit].insert(def);
                defToVertexMap[def]= *vit;
            }
        }
        // __EXIT__
        auto useIt = vinfo.uses.find(Graphs::globalNodeId);
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
        if (src == sink)
            continue;
        //BUG_CHECK(src != sink, "src and sink should be different");
        add_defuse_edge(g, src, sink, edgeVars);
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
            auto defIt = vinfo.defs.find(Graphs::globalNodeId);
            if (defIt != vinfo.defs.end()) {
                auto defSet = defIt->second;
                std::cout << vinfo.name << "(" << defSet.size() << "): "
                          << join_var_names(defSet, false) << std::endl;
            }
        } else {
            std::cout << vinfo.name << std::endl; // print name
            // Normal IR nodes
            for (auto &nid : vinfo.nodes) {
                auto defIt = vinfo.defs.find(nid);
                const varset_t defSet = defIt == vinfo.defs.end() ?
                                        emptyVarSet : defIt->second;
                auto useIt = vinfo.uses.find(nid);
                const varset_t useSet = useIt == vinfo.uses.end() ?
                                        emptyVarSet : useIt->second;

                std::stringstream sstream;
                nid.node->dbprint(sstream);

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
