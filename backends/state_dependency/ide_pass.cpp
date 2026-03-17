#include "ide_pass.h"

namespace P4::P4StateDependency {
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

void IDEPass::collect_all_dep_edges(Tabulation *tab, Graphs::vertex_t v) {
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
            TabEdge te = {paramTv, TabVertex{v, var}};
            foundDepEdges[graphName].push_back(convert_to_var_edge(te));
        }
        // Find if any variable is a member of var
        for (auto mem : get_var_members(tab, var)) {
            auto varBitMap = tab->valueMap[TabVertex{v, mem}];
            for (auto paramTv : tab->get_target_vars(varBitMap)) {
                TabEdge te = {paramTv, TabVertex{v, mem}};
                foundDepEdges[graphName].push_back(convert_to_var_edge(te));
            }
        }
    }
}

std::vector<Graphs::vertex_t> IDEPass::find_next_cfg_node(Graph *g, Graphs::vertex_t v) {
    std::vector<Graphs::vertex_t> foundNodes;
    for (auto [ei, ei_end] = boost::out_edges(v, *g); ei != ei_end; ++ei) {
        auto &edge = (*g)[*ei];
        if (edge.type == EdgeType::CONTROL)
            foundNodes.push_back(boost::target(*ei, *g));
    }
    return foundNodes;
}

}  // namespace P4::P4StateDependency
