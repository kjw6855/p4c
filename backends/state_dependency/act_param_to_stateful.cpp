#include "act_param_to_stateful.h"
#include "graphs.h"

namespace P4::P4StateDependency {

/*
void FindActParamToStateful::collect_action_params(Tabulation *tab,
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
*/

std::vector<const IR::Node *> FindActParamToStateful::get_var_members(Tabulation *tab, const IR::Node *var) {
    // TODO: support others
    auto *varDecl = var->to<IR::Declaration>();
    if (!varDecl) return {};

    std::vector<const IR::Node *> foundVars;
    for (auto gVar : tab->sgProp->progVarInfo.get_all_vars()) {
        if (auto *sl = gVar->to<IR::Slice>()) {
            if (!sl->e0->is<IR::PathExpression>()) continue;
            auto *pe = sl->e0->to<IR::PathExpression>();
            if (auto *decl = refMap->getDeclaration(pe->path)) {
                if (decl == varDecl) {
                    foundVars.push_back(sl);
                    continue;
                }
            }
        }
    }

    return foundVars;
}

cstring dump_found_dependency(Tabulation *tab, TabEdge te, const cstring &prefix) {
    std::stringstream sstream;

    sstream << prefix;

    auto *g = tab->g;
    auto srcit = te.first.node;
    auto &srcinfo = (*g)[srcit];
    if (hasFlag(srcinfo.flags, VertexFlags::ACTION) && srcinfo.node->is<IR::P4Action>()) {
        // Some ACTION vertex has INPUT as its name, not action name
        auto *actNode = srcinfo.node->to<IR::P4Action>();
        sstream << actNode->getName();
        sstream << "(" << srcit <<  "):";
        auto srcVarIt = tab->get_vertex_id(te.first);
        auto srcVarInfo = (*g)[srcVarIt];
        sstream << srcVarInfo.name;
    } else {
        sstream << tab->dump_tab_vertex(te.first);
    }

    sstream << "->" << tab->dump_tab_vertex(te.second);

    return cstring(sstream);
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
                    auto resultStr = dump_found_dependency(tab,
                            {paramTv, TabVertex{*vit, var}}, caseString);
                    std::cout << resultStr << std::endl;
                }
                // Find if any variable is a member of var
                for (auto mem : get_var_members(tab, var)) {
                    size_t actionBitMap = tab->valueMap[TabVertex{*vit, mem}];
                    for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                        auto resultStr = dump_found_dependency(tab,
                                {paramTv, TabVertex{*vit, mem}}, caseString);
                        std::cout << resultStr << std::endl;
                    }
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
                    auto resultStr = dump_found_dependency(tab,
                            {paramTv, useTv}, caseString);
                    std::cout << resultStr << std::endl;
                }
                // Find if any variable is a member of var
                for (auto mem : get_var_members(tab, var)) {
                    size_t actionBitMap = tab->valueMap[TabVertex{*vit, mem}];
                    for (auto paramTv : sgProp->get_action_params(actionBitMap)) {
                        auto resultStr = dump_found_dependency(tab,
                                {paramTv, TabVertex{*vit, mem}}, caseString);
                        std::cout << resultStr << std::endl;
                    }
                }
            }
        } else if (hasFlag(vinfo.flags, VertexFlags::KEY)) {
            // TODO: Check READ_DATA->MATCH
            continue;
        }
    }
}

Visitor::profile_t FindActParamToStateful::init_apply(const IR::Node *n) {
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};

        analyze_control_graph(tab);
    }

    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
