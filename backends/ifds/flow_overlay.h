#ifndef BACKENDS_IFDS_FLOW_OVERLAY_H_
#define BACKENDS_IFDS_FLOW_OVERLAY_H_

#include <vector>

#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"

namespace P4::IFDS {

class Tabulation;

/// Instance-specific IFDS flow functions layered on top of the shared exploded supergraph.
///
/// SuperGraphs builds the base flow (def-use) once per program and every analysis instance solves
/// over the same graph, so an instance that needs extra fact propagation (gen) or must block an
/// existing one (kill) records the change here and undoes it with restore() once its results are
/// collected. commit() keeps the changes (e.g. to draw them) and just forgets the bookkeeping.
class FlowOverlay {
 public:
    /// Gen: add fact edge @p fromVar -> @p toVar (variable vertices of @p g).
    Graphs::edge_t add_flow(Graphs::Graph *g, Graphs::vertex_t fromVar, Graphs::vertex_t toVar,
                            EdgeType type = EdgeType::IFDS);
    /// Gen: add fact edge @p from -> @p to.
    Graphs::edge_t add_flow(Tabulation *tab, const TabVertex &from, const TabVertex &to,
                            EdgeType type = EdgeType::IFDS);

    /// Kill: remove the fact edge @p fromVar -> @p toVar if it exists. Returns whether one was
    /// removed. restore() re-adds it with its original properties (at the end of the edge lists).
    bool kill_flow(Graphs::Graph *g, Graphs::vertex_t fromVar, Graphs::vertex_t toVar);
    bool kill_flow(Tabulation *tab, const TabVertex &from, const TabVertex &to);

    /// Undo every add_flow (in insertion order), then re-add every killed edge.
    void restore();
    /// Keep the current graph and drop the bookkeeping.
    void commit();

    bool empty() const { return added.empty() && killed.empty(); }

 private:
    struct AddedEdge {
        Graphs::Graph *g;
        Graphs::vertex_t from, to;
    };
    struct KilledEdge {
        Graphs::Graph *g;
        Graphs::vertex_t from, to;
        EdgeTypeIface props;
        Graphs::GraphvizAttributes attrs;
    };

    std::vector<AddedEdge> added;
    std::vector<KilledEdge> killed;
};

}  // namespace P4::IFDS

#endif /* BACKENDS_IFDS_FLOW_OVERLAY_H_ */
