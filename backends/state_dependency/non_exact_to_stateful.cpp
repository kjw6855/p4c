#include "non_exact_to_stateful.h"
#include "graphs.h"

namespace P4::P4StateDependency {

void FindNonExactToStateful::collect_non_exact_fields(Tabulation *tab,
        hvec_map<Graphs::vertex_t, std::vector<const IR::Node *>> &fields) {

    auto *g = tab->g;
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        // find table vertices
        auto &vinfo = (*g)[*vit];

        if (!hasFlag(vinfo.flags, VertexFlags::KEY)) continue;
        if (vinfo.node == nullptr || !vinfo.node->is<IR::Key>())
            continue;

        auto key = vinfo.node->to<IR::Key>();
        for (auto elVec : key->keyElements) {
            if (elVec->matchType->path->name.name == "exact"_cs)
                continue;

            auto elVar = elVec->to<IR::KeyElement>()->expression;
            fields[*vit].push_back(elVar);
        }
    }
}

void FindNonExactToStateful::analyze_control_graph(Tabulation *tab) {
    hvec_map<Graphs::vertex_t, std::vector<const IR::Node *>> tabFields;
    collect_non_exact_fields(tab, tabFields);

    if (tabFields.size() == 0) {
        std::cout << "No non-exact match fields" << std::endl;
        return;
    }

    tab->init_ide();
    if (genSupergraphs == GenSGMode::ON_DEMAND)
        tab->forward_tabulate_on_demand_ide();
    else
        tab->forward_tabulate_ide();
    tab->compute_values_ide();
    //tab->dump_result();

    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto &vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
            // Check ACTION->IDX with WRITE_DATA
            // TODO: check WRITE
            for (auto var : vinfo.useVars) {
                size_t actionBitMap = tab->valueMap[TabVertex{*vit, var}];
                for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                    // TODO: find src var
                    std::cout << "A->I: " << tab->dump_tab_edge({
                        paramTv, TabVertex{*vit, var}})
                        << std::endl;
                }
            }
        } else if (hasFlag(vinfo.flags, VertexFlags::SO_WRITE_DATA)) {
            // Check ACTION->DATA
            for (auto var : vinfo.useVars) {
                size_t actionBitMap = tab->valueMap[TabVertex{*vit, var}];
                for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                    // TODO: find src var
                    std::cout << "A->D: " << tab->dump_tab_edge({
                        paramTv, TabVertex{*vit, var}})
                        << std::endl;
                }
            }
        } else if (hasFlag(vinfo.flags, VertexFlags::KEY)) {
            // TODO: Check READ_DATA->MATCH
        }
    }
}

Visitor::profile_t FindNonExactToStateful::init_apply(const IR::Node *n) {
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};

        analyze_control_graph(tab);
    }

    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
