#ifndef BACKENDS_IFDS_IDE_PASS_H_
#define BACKENDS_IFDS_IDE_PASS_H_

#include <functional>

#include "backends/ifds/domain.h"
#include "backends/ifds/edge_func_provider.h"
#include "backends/ifds/flow_overlay.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "ir/ir.h"

namespace P4::IFDS {

using Graph = Graphs::Graph;

class IDEPass : public Graphs,
                public Inspector {
 public:
    explicit IDEPass(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : refMap(refMap),
          typeMap(typeMap),
          controlGraphsArray(controlGraphsArray),
          graphProps(graphProps),
          genSupergraphs(genSupergraphs) {}

    virtual Visitor::profile_t init_apply(const IR::Node *) = 0;
    void set_edge_func();

    using DepEdgeMap = hvec_map<Graphs::VarVertex, std::vector<Graphs::VarVertex>>;
    using FactFilter = std::function<bool(const TabVertex &, const Graphs::Vertex &)>;

 protected:
    /*
     * An IDE instance is defined by four hooks over the shared exploded supergraph:
     *  - get_seeds():          source facts; each one owns a bit of the VarBitSet value.
     *  - add_flow_functions(): instance-specific fact edges (gen/kill) recorded in an overlay.
     *  - edge_func_provider(): IDE edge functions (how values change along fact edges).
     *  - analyze_control_graph(): calls solve() and reads Tabulation::valueMap to collect sinks.
     */
    // Common analysis method with IFDS/IDE
    virtual void analyze_control_graph(Tabulation *tab) = 0;
    // Seed facts of this instance in tab's graph
    virtual std::vector<TabVertex> get_seeds(Tabulation *tab) = 0;
    // Add instance-specific flow (fact edges) to the supergraph before solving. Changes must go
    // through @overlay so analyze_control_graph() can undo them with flowOverlay.restore().
    virtual void add_flow_functions(Tabulation *, const std::vector<TabVertex> &/*seeds*/,
                                    FlowOverlay &/*overlay*/) {}
    // IDE edge functions of this instance
    virtual const EdgeFuncProvider &edge_func_provider() const { return defaultEdgeFuncProvider(); }
    // Set EdgeFunc in graph without solving (used to draw the graph); keeps the added flow
    virtual void set_edge_func_in_graph(Tabulation *tab);
    // Seed, add flow, set edge functions, and run both IDE phases; results land in tab->valueMap
    void solve(Tabulation *tab, const std::vector<TabVertex> &seeds);
    // Multi-domain solve: lay out @domains' seeds in one value, let every domain add its flow,
    // and run both IDE phases once. Returns false (nothing solved) when no domain has a seed.
    // Read one domain's results with DomainSet::targets or the domain-filtered collectors, then
    // undo the domains' flow with flowOverlay.restore().
    bool solve(Tabulation *tab, DomainSet &domains);

    // Find dependency from src var for every dst vertex
    void collect_all_dep_edges(Tabulation *tab, Graphs::vertex_t v, bool isSrcDstMap=true);
    // Find dependency to the facts at v selected by isSink (default: header, parser-derived
    // metadata, and egress port variables)
    void collect_all_dep_edge_to_hdr(Tabulation *tab, Graphs::vertex_t v);
    void collect_all_dep_edge_to_hdr(Tabulation *tab, Graphs::vertex_t v, const FactFilter &isSink);
    // Domain-filtered variants of the two collectors above: only seeds of domain @d count, and
    // the edges go to @out instead of foundDepEdges.
    void collect_dep_edges(Tabulation *tab, Graphs::vertex_t v, const DomainSet &domains,
                           size_t d, DepEdgeMap &out, bool isSrcDstMap=true);
    void collect_dep_edges_to(Tabulation *tab, Graphs::vertex_t v, const FactFilter &isSink,
                              const DomainSet &domains, size_t d, DepEdgeMap &out);
    std::vector<const IR::Node *> get_var_members(Tabulation *tab, const IR::Node *var);
    std::vector<cstring> get_tables_from_action(Tabulation *tab, Graphs::vertex_t action_v);
    std::optional<Graphs::vertex_t> get_table_key(Tabulation *tab, Graphs::vertex_t table_v);
    cstring dump_found_dependency(Tabulation *tab, const Graphs::VarEdge &ve);

    // Shared bodies of the collectors: @targets turns a value into seeds, @out yields the map.
    using TargetsFn = std::function<std::vector<TabVertex>(const VarBitSet &)>;
    using OutFn = std::function<DepEdgeMap &()>;
    void collect_dep_edges_impl(Tabulation *tab, Graphs::vertex_t v, bool isSrcDstMap,
                                const TargetsFn &targets, const OutFn &out);
    void collect_dep_edges_to_impl(Tabulation *tab, Graphs::vertex_t v, const FactFilter &isSink,
                                   const TargetsFn &targets, const OutFn &out);

    inline Graphs::VarEdge convert_to_var_edge(const TabEdge &te) {
        return {{te.first.node, te.first.var},
                {te.second.node, te.second.var}};
    }

    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp *> *graphProps{};
    GenSGMode genSupergraphs;
    FlowOverlay flowOverlay;

 public:
    hvec_map<cstring, DepEdgeMap> foundDepEdges;
};

}  // namespace P4::IFDS

#endif /* BACKENDS_IFDS_IDE_PASS_H_ */
