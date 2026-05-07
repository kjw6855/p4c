#include "stateful_to_cond.h"

#include "graphs.h"

namespace P4::P4StateDependency {

// Call when setting EdgeFunc in graph for visualization.
void FindStatefulToCond::set_edge_func_in_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto graphName = boost::get_property(*g, boost::graph_name);

    if (!stateVars || stateVars->find(graphName) == stateVars->end()) {
        LOG3("Warning: No state variables found for graph '" << graphName << "'");
        return;
    }

    LOG3("Using stateVars for graph '" << graphName << "' with "
         << (*stateVars)[graphName].size() << " variables");
    tab->init_edge_func((*stateVars)[graphName]);
}

void FindStatefulToCond::analyze_control_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto graphName = boost::get_property(*g, boost::graph_name);

    LOG3("Analyzing control graph '" << graphName << "' for S2C");

    BUG_CHECK(tab->sanity_check_ide(), "Invalid ESG for IDE");

    if (!stateVars || stateVars->find(graphName) == stateVars->end()) {
        LOG2("No state variables found for graph '" << graphName << "', skipping S2C");
        return;
    }

    auto curStateVars = (*stateVars)[graphName];
    if (curStateVars.empty()) {
        LOG2("No state variables in " << graphName);
        return;
    }

    auto prevDepEdgeMap = prevDepEdgeMaps ? (*prevDepEdgeMaps)[graphName] : IDEPass::DepEdgeMap{};

    tab->init_edge_func(curStateVars);
    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();

    std::cout << "================" << std::endl;
    std::cout << "[RESULT]";
    if (analysisType == "A2S2V"_cs) std::cout << " Action Parameters ->";
    else if (analysisType == "H2S2V"_cs) std::cout << " Headers ->";
    std::cout << " Stateful Variables -> Conditions in " << graphName << ":\n";

    auto mainProcName = sgProp->procOf[sgProp->rootVar];
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];

        // Collect dep edges arriving at condition-check vertices.
        // CONDITION: if-statement guards with non-table-hit/miss expressions.
        // SWITCH: switch expressions (action_run switches have empty useVars
        //         so they produce no results naturally).
        if ((hasFlag(vinfo.flags, VertexFlags::CONDITION) ||
             hasFlag(vinfo.flags, VertexFlags::SWITCH)) &&
            !vinfo.useVars.empty()) {
            collect_all_dep_edges(tab, *vit);
        }
    }

    for (auto &de : foundDepEdges[graphName]) {
        auto &src = de.first;
        std::stringstream sstream;
        sstream << "[SO->] " << tab->dump_tab_vertex(TabVertex{src.first, src.second});

        auto prevDepEdgeMapIt = prevDepEdgeMap.find(src);
        if (prevDepEdgeMapIt != prevDepEdgeMap.end()) {
            bool init = true;
            for (auto &prevDst : prevDepEdgeMapIt->second) {
                if (init) {
                    init = false;
                    sstream << " ... ";
                } else {
                    sstream << ", ";
                }
                if (prevDst.first == 0) sstream << prevDst.second;
                else sstream << tab->dump_tab_vertex(TabVertex{prevDst.first, prevDst.second});
            }
        }
        sstream << "\n";

        for (auto &dst : de.second) {
            auto dstInfo = (*g)[dst.first];
            if (hasFlag(dstInfo.flags, VertexFlags::CONDITION)) {
                sstream << "  [->COND] "
                    << tab->dump_tab_var_name(TabVertex{dst.first, dst.second})
                    << " @COND:" << dstInfo.name << "\n";
            } else if (hasFlag(dstInfo.flags, VertexFlags::SWITCH)) {
                sstream << "  [->SWITCH] "
                    << tab->dump_tab_var_name(TabVertex{dst.first, dst.second})
                    << " @SWITCH:" << dstInfo.name << "\n";
            } else {
                sstream << "  [->?] "
                    << tab->dump_tab_var_name(TabVertex{dst.first, dst.second}) << "\n";
            }
        }
        std::cout << sstream.str();
    }

    tab->clear_edge_func();
    std::cout << "================" << std::endl << std::endl;
}

Visitor::profile_t FindStatefulToCond::init_apply(const IR::Node *n) {
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
