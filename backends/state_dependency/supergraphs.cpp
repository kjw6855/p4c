#include <cstdint>
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

    add_edge(curProp->rootVar, curProp->progVarInfo[rootNode][0],
            cstring::empty, EdgeType::IFDS_FT);
}

void SuperGraphs::create_var_vertices(const cstring &graphName) {
    auto graphVarIt = graphVars->find(graphName);
    if (graphVarIt == graphVars->end()) BUG("Graph vars are not found: %1%", graphName);
    auto curGraphVars = graphVarIt->second;

    // Find if there is a local variable map
    auto graphLocalVarIt = graphLocalVars->find(graphName);
    VarMap curLocalVars;
    if (graphLocalVarIt != graphLocalVars->end())
        curLocalVars = graphLocalVarIt->second;

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

    auto actionMapIt = actionMaps->find(graphName);
    if (actionMapIt == actionMaps->end()) {
        curProp->actionMap = Graphs::ActionMap();
    } else {
        curProp->actionMap = actionMapIt->second;
    }

    auto ingressPortVarIt = ingressPortVars->find(graphName);
    curProp->ingressPortVar = ingressPortVarIt != ingressPortVars->end() ? ingressPortVarIt->second : nullptr;

    auto egressPortVarIt = egressPortVars->find(graphName);
    curProp->egressPortVar = egressPortVarIt != egressPortVars->end() ? egressPortVarIt->second : nullptr;

    auto dropVarIt = dropVars->find(graphName);
    curProp->dropVar = dropVarIt != dropVars->end() ? dropVarIt->second : nullptr;

    // Add root and global variables
    auto &progVarInfo = curProp->progVarInfo;
    progVarInfo.add_var(Graphs::globalNode);
    for (auto *node : curGraphVars)
        progVarInfo.add_var(node);

    // Add local vars
    for (auto lvpair : curLocalVars) {
        for (auto *node : lvpair.second) {
            progVarInfo.add_var(node, lvpair.first);
        }
    }

    // For each vertex, create VAR vertex ID
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        // For every accessible variables in current procedure
        // (accessible variables = 0 + global (+ local))
        auto &procName = curProp->procOf[*vit];
        for (auto *node : progVarInfo.get_all_vars(procName)) {
            auto varNode = add_var_vertex(node, *vit);
            progVarInfo.push_var_vertex_id(*vit, varNode);
        }
    }
}

void SuperGraphs::gen_supergraph(Graph *g_, SuperGraphProp *sgProp) {
    // Init
    g = g_;
    curProp = sgProp;

    auto vertices = boost::vertices(*g);
    auto graphName = boost::get_property(*g, boost::graph_name);

    // Create <node, var> vertices for global + local variables
    create_var_vertices(graphName);
    create_root_var_vertex();

    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        auto vProcName = curProp->procOf[*vit];
        // Set srcOf
        if (hasFlag(vinfo.flags, VertexFlags::ENTRY)) {
            // TODO: move rootVar to __START__'s var, not special one
            if (curProp->procOf[curProp->rootVar] == vProcName) {
                curProp->srcOf[vProcName] = curProp->rootVar;
            } else {
                curProp->srcOf[vProcName] = *vit;
            }
        }

        // Set actionParams
        if (hasFlag(vinfo.flags, VertexFlags::ACTION)) {
            // Store each defVar in ACTION statements
            for (auto dv : vinfo.defVars) {
                auto dvTv = TabVertex{*vit, dv};
                curProp->actionParams.push_back(dvTv);
            }
        }
    }

    // Create retArg edges
    auto retArgEdgeIt = retArgEdges->find(graphName);
    if (retArgEdgeIt != retArgEdges->end()) {
        curProp->retArgEdges = retArgEdgeIt->second;
        for (auto varEdge : retArgEdgeIt->second) {
            // <e_p, ret> -> <ret, val>
            // TODO: optimize this with the help of TabVertex
            auto srcit = varEdge.first.first;
            auto *srcVar = varEdge.first.second;
            auto srcProcName = curProp->procOf[srcit];
            auto srcVarIdx = curProp->progVarInfo.get_var_index(srcVar, srcProcName);
            auto srcInfo = (*g)[srcit];
            auto dstit = varEdge.second.first;
            auto *dstVar = varEdge.second.second;
            auto dstProcName = curProp->procOf[dstit];
            auto dstVarIdx = curProp->progVarInfo.get_var_index(dstVar, dstProcName);
            auto dstInfo = (*g)[dstit];

            BUG_CHECK(hasFlag(srcInfo.flags, VertexFlags::EXIT),
                    "%1%(%2%) is not EXIT", srcInfo.name, srcit);
            BUG_CHECK(hasFlag(dstInfo.flags, VertexFlags::RETURN),
                    "%1%(%2%) is not RETURN", dstInfo.name, dstit);

            // Directly create callee-to-caller retArg edges
            // TODO: check with globalDefBits
            add_edge(curProp->progVarInfo[srcit][srcVarIdx],
                    curProp->progVarInfo[dstit][dstVarIdx],
                    cstring::empty, EdgeType::IFDS);
        }
    }

    // Traverse ICFG
    std::size_t n = num_vertices(*g);
    auto index = boost::get(boost::vertex_index, *g);
    std::vector<bool> visited(n, false);
    std::queue<Graphs::vertex_t> q;

    // isGlobalsDefined[vid][varIdx] = true if varIdx is defined in the path from entry to vertex vid
    std::vector<boost::dynamic_bitset<uint64_t>> globalDefBits(n,
        boost::dynamic_bitset<uint64_t>(curProp->progVarInfo.get_var_num()));
    boost::dynamic_bitset<uint64_t> emptyBitSet(curProp->progVarInfo.get_var_num());

    hvec_map<Graphs::vertex_t, Graphs::vertex_t> retToCall;
    for (auto &ce : sgProp->callMap) {
        auto [_, callerRet] = ce.second;
        retToCall[callerRet] = ce.first;
    }

    q.push(get_root_vertex(g));
    while (!q.empty()) {
        auto u = q.front();
        auto uid = index[u];
        q.pop();

        for (auto [ei, ei_end] = boost::out_edges(u, *g); ei != ei_end; ++ei) {
            auto &edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR) continue;
            if (edge.type == EdgeType::IFDS) continue;
            if (edge.type == EdgeType::IFDS_FT) continue;
            auto v = boost::target(*ei, *g);
            auto vid = index[v];
            auto vinfo = (*g)[v];

            // Use bitset of source vertex for current path
            boost::dynamic_bitset<uint64_t> curGlobalDefBits;
            if (visited[vid]) {
                // If target is already visited, create new bitset for this path
                curGlobalDefBits = globalDefBits[uid];
            } else {
                // Copy from source vertex's bitset
                globalDefBits[vid] = globalDefBits[uid];
                curGlobalDefBits = globalDefBits[vid];
            }

            // main
            LOG5(u << "(" << uid << ")->" << v << "(" << vid << "): " << edgeTypeToString(edge.type));
            if (edge.type == EdgeType::CALL_TO_RETURN) {
                // Skip Call-to-Return edge, since ifds edges should be generated after calculating procedure
                continue;

            } else if (edge.type == EdgeType::INTER_PROCEDURE && hasFlag(vinfo.flags, VertexFlags::RETURN)) {
                // (1) Exit-to-Return edge
                gen_ifds_edge(u, v, curGlobalDefBits, emptyBitSet);
                auto caller = retToCall[v];

                // (2) Call-to-Return edge
                // We don't have to calculate AND
                gen_ifds_edge(caller, v, {}, curGlobalDefBits);

            } else {
                gen_ifds_edge(u, v, curGlobalDefBits, emptyBitSet);
            }

            if (!visited[vid]) {
                visited[vid] = true;
                globalDefBits[vid] = curGlobalDefBits;
                q.push(v);
            } else {
                // Calculate overwritten globals for already visited path
                globalDefBits[vid] &= curGlobalDefBits;
            }
        }
    }
}

void SuperGraphs::gen_ifds_edge(Graphs::vertex_t src, Graphs::vertex_t dst,
        std::optional<std::reference_wrapper<boost::dynamic_bitset<uint64_t>>> curGlobalDefBits,
        const boost::dynamic_bitset<uint64_t> &skipGlobals) {
    auto &progVarInfo = curProp->progVarInfo;
    auto &srcProcName = curProp->procOf[src];
    auto &dstProcName = curProp->procOf[dst];
    auto &variables = progVarInfo.get_all_vars(dstProcName);
    BUG_CHECK(progVarInfo.get_var_num(dstProcName) == variables.size(),
            "var_num is not consistent with variables size for proc %1%: var_num=%2%, variables.size()=%3%",
            dstProcName, progVarInfo.get_var_num(dstProcName), variables.size());

    LOG2(src << "->" << dst << ": " << (curGlobalDefBits.has_value() ?
            boost::to_string(curGlobalDefBits->get()) : std::string("null")));
    // Add 0->0
    add_edge(progVarInfo[src][0], progVarInfo[dst][0],
             cstring::empty, EdgeType::IFDS_FT);

    auto dstInfo = (*g)[dst];
    // Set drop flag if dst is marked as drop or dst's defVar is dropVar
    bool isDrop = hasFlag(dstInfo.flags, VertexFlags::DROP);
    if (!isDrop && curProp->dropVar) {
        for (size_t n = 1; n < progVarInfo.get_var_num(dstProcName); n++) {
            auto *var = variables[n];
            if (std::find(dstInfo.defVars.begin(), dstInfo.defVars.end(), var)
                    == dstInfo.defVars.end())
                continue;
            if (curProp->dropVar->equiv(*var)) {
                isDrop = true;
                break;
            }
        }
    }

    for (size_t n = 1; n < progVarInfo.get_var_num(dstProcName); n++) {
        // Skip if the global variable is overwritten in the callee
        if (!progVarInfo.is_local(n) && skipGlobals.test(n)) continue;

        auto *var = variables[n];
        // Fall through if var is not newly defined
        if (std::find(dstInfo.defVars.begin(), dstInfo.defVars.end(), var)
                == dstInfo.defVars.end()) {
            if (isDrop && curProp->egressPortVar && var->equiv(*curProp->egressPortVar)) {
                // If dst has drop flag, 0 -> egressVar
                add_edge(progVarInfo[src][0], progVarInfo[dst][n],
                         cstring::empty, EdgeType::IFDS);
                if (curGlobalDefBits.has_value() && !progVarInfo.is_local(n)) {
                    curGlobalDefBits->get().set(n);
                }

            } else if (srcProcName == dstProcName || !progVarInfo.is_local(n)) {
                // Only for global variables and same-proc variables
                add_edge(progVarInfo[src][n], progVarInfo[dst][n],
                         cstring::empty, EdgeType::IFDS_FT);
            }
            continue;
        }

        // TODO: check variable can be overwritten or not
        curProp->defBy[var] = dst;

        // Create new edge
        if (hasFlag(dstInfo.flags, VertexFlags::STATEFUL)) {
            // 0 -> DEF (e.g., v = READ(idx))
            add_edge(progVarInfo[src][0], progVarInfo[dst][n],
                     cstring::empty, EdgeType::IFDS);
            if (curGlobalDefBits.has_value() && !progVarInfo.is_local(n)) {
                curGlobalDefBits->get().set(n);
            }
        } else if (dstInfo.useVars.size() == 0) {
            // 0 -> DEF
            auto edgeId = add_edge(progVarInfo[src][0], progVarInfo[dst][n],
                     cstring::empty, EdgeType::IFDS);
            if (curGlobalDefBits.has_value() && !progVarInfo.is_local(n)) {
                curGlobalDefBits->get().set(n);
            }
        } else {
            // USE -> DEF
            for (auto uv : dstInfo.useVars) {
                auto uvi = progVarInfo.get_var_index(uv, srcProcName);
                add_edge(progVarInfo[src][uvi], progVarInfo[dst][n],
                         cstring::empty, EdgeType::IFDS);
                if (curGlobalDefBits.has_value() && !progVarInfo.is_local(n)) {
                    curGlobalDefBits->get().set(n);
                }
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
