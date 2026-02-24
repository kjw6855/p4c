#include "graphs.h"

namespace P4::P4StateDependency {
Graphs::vertex_t Graphs::add_vertex(const cstring &name, VertexFlags flags, const IR::Node *node) {
    auto v = boost::add_vertex(*g);
    boost::put(&Vertex::name, *g, v, name);
    boost::put(&Vertex::flags, *g, v, flags);
    if (node != nullptr)
        boost::put(&Vertex::node, *g, v, node);

    return g->local_to_global(v);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name, EdgeType type) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&EdgeTypeIface::name, g->root(), ep.first, name);
    boost::put(&EdgeTypeIface::type, g->root(), ep.first, type);
}

void Graphs::add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                      EdgeType type, unsigned cluster_id) {
    auto ep = boost::add_edge(from, to, g->root());
    boost::put(&EdgeTypeIface::name, g->root(), ep.first, name);
    boost::put(&EdgeTypeIface::type, g->root(), ep.first, type);

    auto attrs = boost::get(boost::edge_attribute, g->root());

    attrs[ep.first]["ltail"_cs] = "cluster"_cs + Util::toString(cluster_id - 2);
    attrs[ep.first]["lhead"_cs] = "cluster"_cs + Util::toString(cluster_id - 1);
}

Graphs::vertex_t Graphs::add_and_connect_vertex(const cstring &name, VertexFlags flags,
                                                const IR::Node *node) {
    // merge_other_statements_into_vertex();
    auto v = add_vertex(name, flags, node);
    for (auto parent : parents) add_edge(parent.first, v, parent.second->name, parent.second->type);
    return v;
}

void Graphs::add_and_connect_vertex(Graphs::vertex_t &target, EdgeType edgeType) {
    for (auto parent : parents) add_edge(parent.first, target, parent.second->name, edgeType);
}

}  // namespace P4::P4StateDependency
