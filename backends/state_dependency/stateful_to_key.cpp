#include "stateful_to_key.h"

#include "frontends/p4/methodInstance.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "graphs.h"

namespace P4::P4StateDependency {

std::vector<const IR::Node *> FindStatefulToKey::find_ret_vars(Tabulation *tab, Graphs::vertex_t ret_v) {

    std::vector<const IR::Node *> foundRetVars;
    for (auto varEdge : tab->sgProp->retArgEdges) {
        if (varEdge.second.first == ret_v)
            foundRetVars.push_back(varEdge.second.second);
    }

    return foundRetVars;
}

std::vector<TabVertex> FindStatefulToKey::collect_state_vars(Tabulation *tab, bool showLog) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto graphName = boost::get_property(*g, boost::graph_name);
    auto peit = ptsEdges->find(graphName);
    if (peit == ptsEdges->end()) return {};

    // Store second VarVertex <> -> <>
    hvec_set<TabVertex, TabVertexHash> stateVarSet;
    for (auto ve : peit->second) {
        auto dstInfo = (*g)[ve.second.first];
        auto dstTvVar = TabVertex{ve.second.first, ve.second.second};
        auto dstTvVarInfo = (*g)[tab->get_vertex_id(dstTvVar)];
        if (hasFlag(dstInfo.flags, VertexFlags::SO_IDX)) {
            if (hasFlag(dstInfo.flags, VertexFlags::CALL)) {
                // If it's procedure, get return value
                auto [_, callerRet] = tab->sgProp->get_call_map(ve.second.first);
                auto callerRetInfo = (*g)[callerRet];
                for (auto retVar : find_ret_vars(tab, callerRet)) {
                    auto retTvVar = TabVertex{callerRet, retVar};
                    auto retTvVarInfo = (*g)[tab->get_vertex_id(retTvVar)];
                    stateVarSet.insert(retTvVar);

                    if (showLog)
                        LOG2("- SO [IDX] " << dstInfo.name << ":" << dstTvVarInfo.name << " -> [DATA] " << callerRetInfo.name << ":" << retTvVarInfo.name);
                }
            } else if (hasFlag(dstInfo.flags, VertexFlags::STATEFUL)) {
                for (auto dit : find_next_cfg_node(g, ve.second.first)) {
                    auto dinfo = (*g)[dit];
                    if (!hasFlag(dinfo.flags, VertexFlags::SO_DATA)) continue;
                    for (auto dv : dinfo.defVars) {
                        auto dTvVar = TabVertex{dit, dv};
                        auto dTvVarInfo = (*g)[tab->get_vertex_id(dTvVar)];

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
                            auto dTvVarInfo = (*g)[tab->get_vertex_id(dTvVar)];

                            stateVarSet.insert(dTvVar);
                            if (showLog)
                                LOG2("- SO [DATA:SRC] " << dstInfo.name << ":" << dstTvVarInfo.name << " -> [DATA:DST] " << dinfo.name << ":" << dTvVarInfo.name);
                        }
                    }
                }
                continue;
            }
            // TODO: Use VarVertex instead of TabVertex
            stateVarSet.insert(TabVertex{ve.second.first, ve.second.second});
            if (showLog)
                LOG2("- SO [DATA] " << dstInfo.name << ":" << dstTvVarInfo.name);
        }
    }

    std::vector<TabVertex> stateVarList(std::begin(stateVarSet),
                std::end(stateVarSet));
    return stateVarList;
}

// Call when setting EdgeFunc in graph for visualization
void FindStatefulToKey::set_edge_func_in_graph(Tabulation *tab) {
    auto stateVars = collect_state_vars(tab);
    tab->init_edge_func(stateVars);
}

void FindStatefulToKey::analyze_control_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto graphName = boost::get_property(*g, boost::graph_name);

    BUG_CHECK(tab->sanity_check_ide(), "Invalid ESG for IDE");

    auto stateVars = collect_state_vars(tab, true);
    if (stateVars.size() == 0) {
        std::cout << "No state variables in " << graphName << std::endl;
        return;
    }

    tab->init_edge_func(stateVars);
    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();
    //tab->dump_result();

    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];
        // TODO: support for condition blocks (e.g., if and switch)
        if (hasFlag(vinfo.flags, VertexFlags::KEY)) {
            collect_all_dep_edges(tab, *vit);
        }
    }

    for (auto &ve : foundDepEdges[graphName]) {
        std::cout << "[SO->KEY] "
            << dump_found_dependency(tab, ve) << std::endl;
    }
    tab->clear_edge_func();
}

Visitor::profile_t FindStatefulToKey::init_apply(const IR::Node *n) {
    foundDepEdges.clear();
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};

        analyze_control_graph(tab);
    }

    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
