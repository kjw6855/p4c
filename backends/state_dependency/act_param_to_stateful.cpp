#include "act_param_to_stateful.h"
#include "graphs.h"

namespace P4::P4StateDependency {

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
                caseString = "[B1/3:A->I] "_cs;
            } else if (hasFlag(vinfo.flags, VertexFlags::STATEFUL) &&
                    hasSOFlag(vinfo.soFlags, SOFlags::READ)) {
                caseString = "[A->I] "_cs;
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
                caseString = "[B3:A->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {
                caseString = "[B3:A->D] "_cs;
            } else if (hasSOFlag(vinfo.soFlags, SOFlags::CREATE)) {
                caseString = "[B2:A->D] "_cs;
            }
            if (caseString.size() == 0) continue;

            caseStrings[*vit] = caseString;
            collect_all_dep_edges(tab, *vit);
        }
    }

    for (auto &ve : foundDepEdges[graphName]) {
        std::cout << caseStrings[ve.second.first]
            << dump_found_dependency(tab, ve) << std::endl;
    }
    tab->clear_edge_func();
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
