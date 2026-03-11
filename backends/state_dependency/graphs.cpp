#include "graphs.h"

namespace P4::P4StateDependency {
using vertex_t = Graphs::vertex_t;
const IR::Node *Graphs::globalNode = new IR::Constant(0);

vertex_t Graphs::add_vertex(const cstring &name, VertexFlags flags, const IR::Node *node) {
    auto v = boost::add_vertex(*g);
    boost::put(&Vertex::name, *g, v, name);
    if (isInLocalProc)
        flags |= VertexFlags::SO_DATA;
    boost::put(&Vertex::flags, *g, v, flags);
    if (node != nullptr)
        boost::put(&Vertex::node, *g, v, node);

    procOfs[graphName][v] = procName;

    return g->local_to_global(v);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                      EdgeType type, std::optional<size_t> actId) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&EdgeTypeIface::name, g->root(), ep.first, name);
    boost::put(&EdgeTypeIface::type, g->root(), ep.first, type);

    if (actId.has_value()) {
        auto &edge = g->root()[ep.first];
        edge.setFunc(std::make_unique<ActionSetFunc>(actId.value()));
    }
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                      EdgeType type, unsigned cluster_id, std::optional<size_t> actId) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&EdgeTypeIface::name, g->root(), ep.first, name);
    boost::put(&EdgeTypeIface::type, g->root(), ep.first, type);

    if (actId.has_value()) {
        auto &edge = g->root()[ep.first];
        edge.setFunc(std::make_unique<ActionSetFunc>(actId.value()));
    }

    auto attrs = boost::get(boost::edge_attribute, g->root());

    attrs[ep.first]["ltail"_cs] = "cluster"_cs + Util::toString(cluster_id - 2);
    attrs[ep.first]["lhead"_cs] = "cluster"_cs + Util::toString(cluster_id - 1);
}

vertex_t Graphs::add_and_connect_vertex(const cstring &name, VertexFlags flags,
                                                const IR::Node *node) {
    // merge_other_statements_into_vertex();
    auto v = add_vertex(name, flags, node);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->name, parent.second->type);
    return v;
}

void Graphs::add_and_connect_vertex(vertex_t &target, EdgeType edgeType) {
    for (auto parent : parents) add_edge(parent.first, target, parent.second->name, edgeType);
}

vertex_t Graphs::add_var_vertex(const IR::Node *var, std::optional<const vertex_t> node,
        std::optional<cstring> name) {
    cstring vname = name.has_value() ? name.value() : get_var_name(var);
    auto vv = add_vertex(vname, VertexFlags::VARIABLE, var);

    if (node.has_value())
        add_edge(node.value(), vv, cstring::empty, EdgeType::HAS_VAR);

    return vv;
}

const IR::Node *Graphs::add_variable_in_vertex(const IR::Node *var,
            const vertex_t &v, bool isUsed,
            std::optional<cstring> name) {
    /* check if the variable was defined as local */
    if (isInLocalProc) {
        auto &varset = graphLocalVars[graphName][procName];
        for (auto *inVar : varset) {
            if (inVar->equiv(*var)) {
                return add_local_variable_in_vertex(var, v, isUsed, name);
            }
        }
    }

    /* check duplicate variable in set */
    auto &varset = graphVars[graphName];
    auto *newVar = var;
    for (auto *inVar : varset) {
        if (inVar->equiv(*var)) {
            newVar = inVar;
            break;
        }
    }

    // Store Graphs::curKeyVars for later uses (e.g., add_entry's indices)
    if (storeKeys) curKeyVars.push_back(newVar);

    // Insert new variable
    if (newVar == var)
        graphVars[graphName].insert(var);

    bool createVar = varVis != VarVisibility::NONE;
    bool genSG = genSupergraphs != GenSGMode::NONE;
    if (createVar || genSG) {
        auto &vinfo = (*g)[v];
        auto &varList = isUsed ? vinfo.useVars : vinfo.defVars;
        varList.push_back(newVar);

        // supergraphs.cpp will create vertex later
        if (createVar && !genSG) {
            add_var_vertex(newVar, v, name);
        }
    }
    return newVar;
}

const IR::Node *Graphs::add_local_variable_in_vertex(const IR::Node *var,
        const vertex_t &v, bool isUsed,
        std::optional<cstring> name) {
    auto &varset = graphLocalVars[graphName][procName];
    auto *newVar = var;
    for (auto *inVar : varset) {
        if (inVar->equiv(*var)) {
            newVar = inVar;
            break;
        }
    }
    if (newVar == var)
        graphLocalVars[graphName][procName].insert(var);

    bool createVar = varVis != VarVisibility::NONE;
    bool genSG = genSupergraphs != GenSGMode::NONE;
    if (createVar || genSG) {
        auto &vinfo = (*g)[v];
        auto &varList = isUsed ? vinfo.useVars : vinfo.defVars;
        varList.push_back(newVar);

        // supergraphs.cpp will create vertex later
        if (createVar && !genSG) {
            add_var_vertex(newVar, v, name);
        }
    }
    return newVar;
}


}  // namespace P4::P4StateDependency
