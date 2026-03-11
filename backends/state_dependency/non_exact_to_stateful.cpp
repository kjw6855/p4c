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
        auto &vProcName = sgProp->procOf[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
            // Check ACTION->IDX
            cstring caseString = cstring::empty;

            // TODO: differentiate stateful CALL and procedures
            // TODO: apply block could be used only for read
            if (!hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
                    hasSOFlag(vinfo.soFlags, SOFlags::UPDATE)) {    //TODO: CREATE
                caseString = "[B1/3:A->I] "_cs;
            }
            if (caseString.size() == 0) continue;
            for (auto var : vinfo.useVars) {
                size_t actionBitMap = tab->valueMap[TabVertex{*vit, var}];
                for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                    std::cout << caseString << tab->dump_tab_edge({
                        paramTv, TabVertex{*vit, var}})
                        << std::endl;
                }
            }
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

            for (auto var : vinfo.useVars) {
                TabVertex useTv{*vit, var};

                // If useVar is local, it's not dependent on global
                // RegisterAction local vars are inout value, out rv,
                // and other declared values, which are not related
                // to dependency analysis.
                if (sgProp->progVarInfo.is_local(var, vProcName))
                    continue;

                size_t actionBitMap = tab->valueMap[useTv];
                for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                    std::cout << caseString << tab->dump_tab_edge({paramTv, useTv})
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
