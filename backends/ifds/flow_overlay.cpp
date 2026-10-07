#include "backends/ifds/flow_overlay.h"

#include "backends/ifds/tabulation.h"

namespace P4::IFDS {

Graphs::edge_t FlowOverlay::add_flow(Graphs::Graph *g, Graphs::vertex_t fromVar,
                                     Graphs::vertex_t toVar, EdgeType type) {
    auto &root = g->root();
    auto ep = boost::add_edge(fromVar, toVar, root);
    boost::put(&EdgeTypeIface::name, root, ep.first, cstring::empty);
    boost::put(&EdgeTypeIface::type, root, ep.first, type);
    added.push_back({g, fromVar, toVar});
    return ep.first;
}

Graphs::edge_t FlowOverlay::add_flow(Tabulation *tab, const TabVertex &from, const TabVertex &to,
                                     EdgeType type) {
    return add_flow(tab->g, tab->get_vertex_id(from), tab->get_vertex_id(to), type);
}

bool FlowOverlay::kill_flow(Graphs::Graph *g, Graphs::vertex_t fromVar, Graphs::vertex_t toVar) {
    auto &root = g->root();
    auto edgeIt = boost::edge(fromVar, toVar, root);
    if (!edgeIt.second) return false;
    auto attrs = boost::get(boost::edge_attribute, root);
    killed.push_back({g, fromVar, toVar, root[edgeIt.first], attrs[edgeIt.first]});
    boost::remove_edge(edgeIt.first, root);
    return true;
}

bool FlowOverlay::kill_flow(Tabulation *tab, const TabVertex &from, const TabVertex &to) {
    return kill_flow(tab->g, tab->get_vertex_id(from), tab->get_vertex_id(to));
}

void FlowOverlay::restore() {
    for (auto &e : added) {
        auto &root = e.g->root();
        auto edgeIt = boost::edge(e.from, e.to, root);
        if (edgeIt.second) boost::remove_edge(edgeIt.first, root);
    }
    for (auto &e : killed) {
        auto &root = e.g->root();
        auto ep = boost::add_edge(e.from, e.to, root);
        root[ep.first] = e.props;
        boost::get(boost::edge_attribute, root)[ep.first] = e.attrs;
    }
    commit();
}

void FlowOverlay::commit() {
    added.clear();
    killed.clear();
}

}  // namespace P4::IFDS
