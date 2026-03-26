#include "act_param_to_stateful.h"

#include <regex>
#include "graphs.h"

namespace P4::P4StateDependency {

std::vector<cstring> get_tables_from_action(Tabulation *tab, Graphs::vertex_t action_v) {
    auto *g = tab->g;
    auto actName = tab->sgProp->procOf[action_v];
    auto actSrcit = tab->sgProp->srcOf[actName];
    // Find all callers
    std::vector<cstring> tables;
    for (auto [ei, ei_end] = boost::in_edges(actSrcit, *g); ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::INTER_PROCEDURE)
            continue;

        auto u = boost::source(*ei, *g);
        auto uinfo = (*g)[u];
        if (hasFlag(uinfo.flags, VertexFlags::CALL)) {
            auto callerName = tab->sgProp->procOf[u];
            auto callerSrcit = tab->sgProp->srcOf[callerName];
            auto callerSrcInfo = (*g)[callerSrcit];
            if (hasFlag(callerSrcInfo.flags, VertexFlags::TABLE))
                tables.push_back(callerName);
        }
    }
    return tables;
}

std::optional<Graphs::vertex_t> get_table_key(Tabulation *tab, Graphs::vertex_t table_v) {
    auto *g = tab->g;
    for (auto [ei, ei_end] = boost::out_edges(table_v, *g); ei != ei_end; ++ei) {
        auto edge = (*g)[*ei];
        if (edge.type != EdgeType::CONTROL)
            continue;

        auto u = boost::target(*ei, *g);
        auto uinfo = (*g)[u];
        if (hasFlag(uinfo.flags, VertexFlags::KEY))
            return u;
    }
    return {};
}

void FindActParamToStateful::set_edge_func_in_graph(Tabulation *tab) {
    tab->init_edge_func(tab->sgProp->actionParams);
}

void FindActParamToStateful::analyze_control_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto graphName = boost::get_property(*g, boost::graph_name);
    if (sgProp->actionParams.size() == 0) {
        std::cout << "No action params in " << graphName << std::endl;
        return;
    }

    BUG_CHECK(tab->sanity_check_ide(), "Invalid ESG for IDE");
    set_edge_func_in_graph(tab);

    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();
    //tab->dump_result();

    std::cout << "================" << std::endl << std::endl;
    std::cout << "[RESULT] Action Parameters -> Stateful Variables in " << graphName << ":\n";
    auto vertices = boost::vertices(*g);
    // Dependency cases for each dst vertex
    hvec_map<Graphs::vertex_t, cstring> caseStrings;
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
            // Check ACTION->IDX
            cstring caseString = cstring::empty;

            // TODO: differentiate stateful CALL and procedures
            // TODO: apply block could be used only for read
            if (!hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
                    hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {    //TODO: CREATE
                caseString = "[B1/3:->I] "_cs;
            } else if (hasFlag(vinfo.flags, VertexFlags::STATEFUL) &&
                    hasSOFlag(vinfo.soFlags, SOFlags::READ)) {
                caseString = "[->I] "_cs;
            }
            if (caseString.size() == 0) continue;

            caseStrings[*vit] = caseString;
            collect_all_dep_edges(tab, *vit);

        } else if (hasFlag(vinfo.flags, VertexFlags::SO_DATA)) {
            // Check ACTION->DATA
            cstring caseString = cstring::empty;

            // TODO: differentiate stateful CALL and procedures
            if (!hasFlag(vinfo.flags, VertexFlags::STATEFUL)) {
                // Assuming it's procedure statements
                caseString = "[B3:->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {
                caseString = "[B3:->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::CREATE)) {
                caseString = "[B2:->D] "_cs;
            }
            if (caseString.size() == 0) continue;

            caseStrings[*vit] = caseString;
            collect_all_dep_edges(tab, *vit);
        }
    }

    for (auto &de : foundDepEdges[graphName]) {
        auto &src = de.first;
        auto tableNames = get_tables_from_action(tab, src.first);
        std::stringstream sstream;
        sstream << "[ACT->] ";
        // 1. Dump src
        for (auto tableName : tableNames) {
            auto tableKey = get_table_key(tab, tab->sgProp->srcOf[tableName]);
            cstring keyString = tableKey.has_value() ? (*g)[*tableKey].name : "<no key>"_cs;
            std::string keyStr = std::regex_replace(keyString.c_str(), std::regex("\\\\n"), ", ");
            // Remove the last ", " if present
            if (keyStr.size() >= 2 && keyStr.substr(keyStr.size() - 2) == ", ") {
                keyStr = keyStr.substr(0, keyStr.size() - 2);
            }
            sstream << "@TABLE:" << tableName << "(" << keyStr << ") ";
        }
        sstream << tab->dump_tab_vertex(TabVertex{src.first, src.second}) << "\n";

        // 2. Dump dst
        for (auto dst : de.second)
            sstream << "  " << caseStrings[dst.first]
                    << tab->dump_tab_vertex(TabVertex{dst.first, dst.second}) << "\n";

        std::cout << sstream.str();
    }
    tab->clear_edge_func();
    std::cout << "================" << std::endl << std::endl;
}

Visitor::profile_t FindActParamToStateful::init_apply(const IR::Node *n) {
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
