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

bool collect_state_vars_common(Graphs::Graph *g, SuperGraphProp *sgProp,
    P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
    Graphs::vertex_t node, const IR::Node *trackedVar,
    hvec_set<TabVertex, TabVertexHash> &stateVarSet, bool showLog) {
    auto nodeInfo = (*g)[node];
    std::optional<cstring> trackedVarName;
    if (trackedVar != nullptr) {
        auto trackedTv = TabVertex{node, trackedVar};
        auto trackedVarInfo = (*g)[get_vertex_id(g, sgProp, trackedTv)];
        trackedVarName = trackedVarInfo.name;
    }

    if (hasFlag(nodeInfo.flags, VertexFlags::SO_IDX)) {
        if (hasFlag(nodeInfo.flags, VertexFlags::CALL)) {
            // If it's procedure, get return value
            auto [_, callerRet] = sgProp->get_call_map(node);
            auto callerRetInfo = (*g)[callerRet];
            for (auto retVar : find_ret_vars(sgProp, callerRet)) {
                auto retTvVar = TabVertex{callerRet, retVar};
                auto retTvVarInfo = (*g)[get_vertex_id(g, sgProp, retTvVar)];
                stateVarSet.insert(retTvVar);

                if (showLog) {
                    if (trackedVarName.has_value()) {
                        LOG2("- SO [IDX] " << nodeInfo.name << ":" << trackedVarName.value()
                            << " -> [DATA] " << callerRetInfo.name << ":" << retTvVarInfo.name);
                    } else {
                        LOG2("- SO [IDX] " << nodeInfo.name
                            << " -> [DATA] " << callerRetInfo.name << ":" << retTvVarInfo.name);
                    }
                }
            }
        } else if (hasFlag(nodeInfo.flags, VertexFlags::STATEFUL)) {
            for (auto dit : find_next_cfg_node(g, node)) {
                auto dinfo = (*g)[dit];
                if (!hasFlag(dinfo.flags, VertexFlags::SO_DATA)) continue;
                for (auto dv : dinfo.defVars) {
                    auto dTvVar = TabVertex{dit, dv};
                    auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                    stateVarSet.insert(dTvVar);
                    if (showLog) {
                        if (trackedVarName.has_value()) {
                            LOG2("- SO [IDX] " << nodeInfo.name << ":" << trackedVarName.value()
                                << " -> [DATA] " << dinfo.name << ":" << dTvVarInfo.name);
                        } else {
                            LOG2("- SO [IDX] " << nodeInfo.name
                                << " -> [DATA] " << dinfo.name << ":" << dTvVarInfo.name);
                        }
                    }
                }
            }
        }
        return true;
    }

    if (hasFlag(nodeInfo.flags, VertexFlags::SO_DATA)) {
        if (hasSOFlag(nodeInfo.soFlags, SOFlags::CREATE) &&
                nodeInfo.node->is<IR::MethodCallStatement>()) {
            // For add_entry(), check the first param to find the hitAction name
            auto stmt = nodeInfo.node->to<IR::MethodCallStatement>();
            auto instance = P4::MethodInstance::resolve(stmt->methodCall, refMap, typeMap);
            if (auto *ec = instance->to<P4::ExternCall>()) {
                // TODO: check if it has to support other methods
                if (ec->method->name.name != "add_entry") return true;

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
                        if (showLog) {
                            if (trackedVarName.has_value()) {
                                LOG2("- SO [DATA:SRC] " << nodeInfo.name << ":" << trackedVarName.value()
                                    << " -> [DATA:DST] " << dinfo.name << ":" << dTvVarInfo.name);
                            } else {
                                LOG2("- SO [DATA:SRC] " << nodeInfo.name
                                    << " -> [DATA:DST] " << dinfo.name << ":" << dTvVarInfo.name);
                            }
                        }
                    }
                }
            }
        }

        if (trackedVar != nullptr) {
            // If trackedVar is given, only collect related state variables
            // TODO: Use VarVertex instead of TabVertex
            stateVarSet.insert(TabVertex{node, trackedVar});
            if (showLog && trackedVarName.has_value())
                LOG2("- SO [DATA] " << nodeInfo.name << ":" << trackedVarName.value());
        } else {
            // Otherwise, collect all state variables related to the node
            for (auto dv : nodeInfo.defVars) {
                auto dTvVar = TabVertex{node, dv};
                auto dTvVarInfo = (*g)[get_vertex_id(g, sgProp, dTvVar)];

                stateVarSet.insert(dTvVar);
                if (showLog)
                    LOG2("- SO [DATA] " << nodeInfo.name << ":" << dTvVarInfo.name);
            }
        }
        return true;
    }

    return false;
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

std::vector<TabVertex> collect_state_vars_from_dep_edges_dst(Graphs::Graph *g, SuperGraphProp *sgProp,
    P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
    const IDEPass::DepEdgeMap &depEdgeMap, bool showLog) {
    // Store second VarVertex <> -> <>
    hvec_set<TabVertex, TabVertexHash> stateVarSet;
    for (auto ve : depEdgeMap) {
        for (auto dst : ve.second) {
            collect_state_vars_common(g, sgProp, refMap, typeMap,
                    dst.first, dst.second, stateVarSet, showLog);
        }
    }

    std::vector<TabVertex> stateVarList(std::begin(stateVarSet),
                std::end(stateVarSet));
    return stateVarList;
}

std::vector<TabVertex> collect_state_vars_from_dep_edges_src(Graphs::Graph *g, SuperGraphProp *sgProp,
    P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
    const IDEPass::DepEdgeMap &depEdgeMap, bool showLog) {
    // Store second VarVertex <> -> <>
    hvec_set<TabVertex, TabVertexHash> stateVarSet;
    for (auto ve : depEdgeMap) {
        auto src = ve.first;
        collect_state_vars_common(g, sgProp, refMap, typeMap,
                src.first, src.second, stateVarSet, showLog);
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
        collect_state_vars_common(g, sgProp, refMap, typeMap,
                *vit, nullptr, stateVarSet, showLog);
    }

    std::vector<TabVertex> stateVarList(std::begin(stateVarSet),
                std::end(stateVarSet));
    return stateVarList;
}
}  // namespace P4::P4StateDependency