#include "utils.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

Graphs::vertex_t get_vertex_id(Graphs::Graph *g, SuperGraphProp *sgProp, const TabVertex &tb) {
    if (tb.node == sgProp->rootVar) return tb.node;
    auto ninfo = (*g)[tb.node];
    BUG_CHECK(!hasFlag(ninfo.flags, VertexFlags::VARIABLE),
                "TabVertex has wrong node Id %1%", tb.node);

    auto idx = sgProp->progVarInfo.get_var_index(tb.var,
            sgProp->procOf[tb.node]);
    return sgProp->progVarInfo[tb.node][idx];
}

std::vector<Graphs::vertex_t> find_next_cfg_node(Graphs::Graph *g, Graphs::vertex_t v) {
    std::vector<Graphs::vertex_t> foundNodes;
    for (auto [ei, ei_end] = boost::out_edges(v, *g); ei != ei_end; ++ei) {
        auto &edge = (*g)[*ei];
        if (edge.type == EdgeType::CONTROL)
            foundNodes.push_back(boost::target(*ei, *g));
    }
    return foundNodes;
}

std::vector<const IR::Node *> find_ret_vars(SuperGraphProp *sgProp, Graphs::vertex_t ret_v) {

    std::vector<const IR::Node *> foundRetVars;
    for (auto varEdge : sgProp->retArgEdges) {
        if (varEdge.second.first == ret_v)
            foundRetVars.push_back(varEdge.second.second);
    }

    return foundRetVars;
}

std::vector<TabVertex> collect_state_vars_from_dep_edges(Graphs::Graph *g, SuperGraphProp *sgProp,
    P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
    const IDEPass::DepEdgeMap &ptsEdgeMap, bool showLog) {
    // Store second VarVertex <> -> <>
    hvec_set<TabVertex, TabVertexHash> stateVarSet;
    for (auto ve : ptsEdgeMap) {
        for (auto dst : ve.second) {
            auto dstInfo = (*g)[dst.first];
            auto dstTvVar = TabVertex{dst.first, dst.second};
            auto dstTvVarInfo = (*g)[get_vertex_id(g, sgProp, dstTvVar)];
            if (hasFlag(dstInfo.flags, VertexFlags::SO_IDX)) {
                if (hasFlag(dstInfo.flags, VertexFlags::CALL)) {
                    // If it's procedure, get return value
                    auto [_, callerRet] = sgProp->get_call_map(dst.first);
                    auto callerRetInfo = (*g)[callerRet];
                    for (auto retVar : find_ret_vars(sgProp, callerRet)) {
                        auto retTvVar = TabVertex{callerRet, retVar};
                        auto retTvVarInfo = (*g)[get_vertex_id(g, sgProp, retTvVar)];
                        stateVarSet.insert(retTvVar);

                        if (showLog)
                            LOG2("- SO [IDX] " << dstInfo.name << ":" << dstTvVarInfo.name << " -> [DATA] " << callerRetInfo.name << ":" << retTvVarInfo.name);
                    }
                } else if (hasFlag(dstInfo.flags, VertexFlags::STATEFUL)) {
                    for (auto dit : find_next_cfg_node(g, dst.first)) {
                        auto dinfo = (*g)[dit];
                        if (!hasFlag(dinfo.flags, VertexFlags::SO_DATA)) continue;
                        for (auto dv : dinfo.defVars) {
                            auto dTvVar = TabVertex{dit, dv};
                            auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                            stateVarSet.insert(dTvVar);
                            if (showLog)
                                LOG2("- SO [IDX] " << dstInfo.name << ":" << dstTvVarInfo.name << " -> [DATA] " << dinfo.name << ":" << dTvVarInfo.name);
                        }
                    }
                }
            } else if (hasFlag(dstInfo.flags, VertexFlags::SO_DATA)) {
                // For add_entry(), check the first param to find the hitAction name
                if (hasSOFlag(dstInfo.soFlags, SOFlags::CREATE) &&
                        dstInfo.node->is<IR::MethodCallStatement>()) {
                    auto stmt = dstInfo.node->to<IR::MethodCallStatement>();
                    auto instance = P4::MethodInstance::resolve(stmt->methodCall,
                            refMap, typeMap);
                    if (auto *ec = instance->to<P4::ExternCall>()) {
                        // TODO: check if it has to support other methods
                        if (ec->method->name.name != "add_entry") continue;

                        auto hitActionName = stmt->methodCall->arguments->at(0)
                            ->expression->to<IR::StringLiteral>()->value;
                        auto hitActionVit = sgProp->actionMap.find(hitActionName);
                        BUG_CHECK(hitActionVit != sgProp->actionMap.end(),
                                "Can't find %1% action", hitActionName);
                        for (auto dit : find_next_cfg_node(g, hitActionVit->second)) {
                            auto dinfo = (*g)[dit];
                            // Find INPUT node, if actionParams exist
                            if (!hasFlag(dinfo.flags, VertexFlags::ACTION)) continue;
                            for (auto dv : dinfo.defVars) {
                                auto dTvVar = TabVertex{dit, dv};
                                auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                                stateVarSet.insert(dTvVar);
                                if (showLog)
                                    LOG2("- SO [DATA:SRC] " << dstInfo.name << ":" << dstTvVarInfo.name << " -> [DATA:DST] " << dinfo.name << ":" << dTvVarInfo.name);
                            }
                        }
                    }
                    continue;
                }
                // TODO: Use VarVertex instead of TabVertex
                stateVarSet.insert(TabVertex{dst.first, dst.second});
                if (showLog)
                    LOG2("- SO [DATA] " << dstInfo.name << ":" << dstTvVarInfo.name);
            }
        }
    }

    std::vector<TabVertex> stateVarList(std::begin(stateVarSet),
                std::end(stateVarSet));
    return stateVarList;
}

std::vector<TabVertex> collect_state_vars(Graphs::Graph *g, SuperGraphProp *sgProp,
    P4::ReferenceMap *refMap, P4::TypeMap *typeMap, bool showLog) {
    auto graphName = boost::get_property(*g, boost::graph_name);
    hvec_set<TabVertex, TabVertexHash> stateVarSet;
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
            if (hasFlag(vinfo.flags, VertexFlags::CALL)) {
                // If it's procedure, get return value
                auto [_, callerRet] = sgProp->get_call_map(*vit);
                auto callerRetInfo = (*g)[callerRet];
                for (auto retVar : find_ret_vars(sgProp, callerRet)) {
                    auto retTvVar = TabVertex{callerRet, retVar};
                    auto retTvVarInfo = (*g)[get_vertex_id(g, sgProp, retTvVar)];
                    stateVarSet.insert(retTvVar);

                    if (showLog)
                        LOG2("- SO [IDX] " << vinfo.name << " -> [DATA] " << callerRetInfo.name << ":" << retTvVarInfo.name);
                }
            } else if (hasFlag(vinfo.flags, VertexFlags::STATEFUL)) {
                for (auto dit : find_next_cfg_node(g, *vit)) {
                    auto dinfo = (*g)[dit];
                    if (!hasFlag(dinfo.flags, VertexFlags::SO_DATA)) continue;
                    for (auto dv : dinfo.defVars) {
                        auto dTvVar = TabVertex{dit, dv};
                        auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                        stateVarSet.insert(dTvVar);
                        if (showLog)
                            LOG2("- SO [IDX] " << vinfo.name << " -> [DATA] " << dinfo.name << ":" << dTvVarInfo.name);
                    }
                }
            }
        } else if (hasFlag(vinfo.flags, VertexFlags::SO_DATA)) {
            // For add_entry(), check the first param to find the hitAction name
            if (hasSOFlag(vinfo.soFlags, SOFlags::CREATE) &&
                    vinfo.node->is<IR::MethodCallStatement>()) {
                auto stmt = vinfo.node->to<IR::MethodCallStatement>();
                auto instance = P4::MethodInstance::resolve(stmt->methodCall,
                        refMap, typeMap);
                if (auto *ec = instance->to<P4::ExternCall>()) {
                    // TODO: check if it has to support other methods
                    if (ec->method->name.name != "add_entry") continue;

                    auto hitActionName = stmt->methodCall->arguments->at(0)
                        ->expression->to<IR::StringLiteral>()->value;
                    auto hitActionVit = sgProp->actionMap.find(hitActionName);
                    BUG_CHECK(hitActionVit != sgProp->actionMap.end(),
                            "Can't find %1% action", hitActionName);
                    for (auto dit : find_next_cfg_node(g, hitActionVit->second)) {
                        auto dinfo = (*g)[dit];
                        // Find INPUT node, if actionParams exist
                        if (!hasFlag(dinfo.flags, VertexFlags::ACTION)) continue;
                        for (auto dv : dinfo.defVars) {
                            auto dTvVar = TabVertex{dit, dv};
                            auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                            stateVarSet.insert(dTvVar);
                            if (showLog)
                                LOG2("- SO [DATA:SRC] " << vinfo.name  << " -> [DATA:DST] " << dinfo.name << ":" << dTvVarInfo.name);
                        }
                    }
                }
                continue;
            }
            for (auto dv : vinfo.defVars) {
                auto dTvVar = TabVertex{*vit, dv};
                auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                stateVarSet.insert(dTvVar);
                if (showLog)
                    LOG2("- SO [DATA] " << vinfo.name << ":" << dTvVarInfo.name);
            }
        }
    }

    std::vector<TabVertex> stateVarList(std::begin(stateVarSet),
                std::end(stateVarSet));
    return stateVarList;
}
}  // namespace P4::P4StateDependency