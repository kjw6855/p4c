#include "ide_pass.h"

namespace P4::P4StateDependency {

std::vector<cstring> IDEPass::get_tables_from_action(Tabulation *tab, Graphs::vertex_t action_v) {
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

std::optional<Graphs::vertex_t> IDEPass::get_table_key(Tabulation *tab, Graphs::vertex_t table_v) {
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

std::vector<const IR::Node *> IDEPass::get_var_members(Tabulation *tab, const IR::Node *var) {
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

cstring IDEPass::dump_found_dependency(Tabulation *tab, const Graphs::VarEdge &ve) {
    std::stringstream sstream;

    auto *g = tab->g;
    auto srcit = ve.first.first;
    auto &srcinfo = (*g)[srcit];
    if (hasFlag(srcinfo.flags, VertexFlags::ACTION) && srcinfo.node->is<IR::P4Action>()) {
        // Some ACTION vertex has INPUT as its name, not action name
        auto *actNode = srcinfo.node->to<IR::P4Action>();
        sstream << actNode->getName();
        sstream << "(" << srcit <<  "):";
        // FIXME: TabVertex to VarVertex
        auto srcVarIt = tab->get_vertex_id(TabVertex{ve.first.first, ve.first.second});
        auto srcVarInfo = (*g)[srcVarIt];
        sstream << srcVarInfo.name;
    } else {
        sstream << tab->dump_tab_vertex(TabVertex{ve.first.first, ve.first.second});
    }

    sstream << "->" << tab->dump_tab_vertex(TabVertex{ve.second.first, ve.second.second});

    return cstring(sstream);
}

void IDEPass::set_edge_func() {
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};

        // call overriden method
        set_edge_func_in_graph(tab);
    }
}

void IDEPass::collect_all_dep_edges(Tabulation *tab, Graphs::vertex_t v, bool isSrcDstMap) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto vinfo = (*g)[v];
    auto graphName = boost::get_property(*g, boost::graph_name);
    auto &vProcName = sgProp->procOf[v];

    // TODO: support IR::Operation (e.g., id * 3)
    for (auto var : vinfo.useVars) {
        /*
         * Local variables are handled in different ways depending
         * on statement types.
         * - SO_DATA (e.g., RegisterAction::apply()): X
         *   Local vars (inout value, out rv) are used for simple
         *   operations within the local procedure.
         * - ACTION: O
         *   Local-var parameters can be used as an argument of SO.
         */
        if (hasFlag(vinfo.flags, VertexFlags::SO_DATA) &&
                sgProp->progVarInfo.is_local(var, vProcName))
            continue;

        auto varBitMap = tab->valueMap[TabVertex{v, var}];
        for (auto paramTv : tab->get_target_vars(varBitMap)) {
            // If isSrcDstMap is true, create map from src (paramTv) to dst (v, var).
            if (isSrcDstMap) {
                foundDepEdges[graphName][{paramTv.node, paramTv.var}].push_back({v, var});
            } else {
                foundDepEdges[graphName][{v, var}].push_back({paramTv.node, paramTv.var});
            }
        }
        // Find if any variable is a member of var
        for (auto mem : get_var_members(tab, var)) {
            auto varBitMap = tab->valueMap[TabVertex{v, mem}];
            for (auto paramTv : tab->get_target_vars(varBitMap)) {
                foundDepEdges[graphName][{paramTv.node, paramTv.var}].push_back({v, mem});
            }
        }
    }
}

void IDEPass::collect_all_dep_edge_to_hdr(Tabulation *tab, Graphs::vertex_t v) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto vinfo = (*g)[v];
    auto graphName = boost::get_property(*g, boost::graph_name);

    for (auto var : sgProp->progVarInfo.get_all_vars()) {
        auto varTv = TabVertex{v, var};
        auto varVit = tab->get_vertex_id(varTv);
        auto varInfo = (*g)[varVit];
        // Skip non-header variables
        // TODO: consider different name for header variables instead of "hdr"
        if (varInfo.name.startsWith("hdr") || (sgProp->egressPortVar && sgProp->egressPortVar->equiv(*var))) {
            auto varBitMap = tab->valueMap[varTv];
            for (auto paramTv : tab->get_target_vars(varBitMap)) {
                foundDepEdges[graphName][{paramTv.node, paramTv.var}].push_back({v, var});
            }
        }
    }
}


}  // namespace P4::P4StateDependency
