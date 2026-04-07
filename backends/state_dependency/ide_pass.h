#ifndef BACKENDS_STATE_DEPENDENCY_IDE_PASS_H_
#define BACKENDS_STATE_DEPENDENCY_IDE_PASS_H_

#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

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

 protected:
    // Common analysis method with IFDS/IDE
    virtual void analyze_control_graph(Tabulation *tab) = 0;
    // Initial method to set EdgeFunc
    virtual void set_edge_func_in_graph(Tabulation *tab) = 0;
    // Find dependency from src var for every dst vertex
    void collect_all_dep_edges(Tabulation *tab, Graphs::vertex_t v, bool isSrcDstMap=true);
    void collect_all_dep_edge_to_hdr(Tabulation *tab, Graphs::vertex_t v);
    std::vector<const IR::Node *> get_var_members(Tabulation *tab, const IR::Node *var);
    std::vector<cstring> get_tables_from_action(Tabulation *tab, Graphs::vertex_t action_v);
    std::optional<Graphs::vertex_t> get_table_key(Tabulation *tab, Graphs::vertex_t table_v);
    cstring dump_found_dependency(Tabulation *tab, const Graphs::VarEdge &ve);
    std::vector<Graphs::vertex_t> find_next_cfg_node(Graph *g, Graphs::vertex_t v);

    inline Graphs::VarEdge convert_to_var_edge(const TabEdge &te) {
        return {{te.first.node, te.first.var},
                {te.second.node, te.second.var}};
    }

    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    std::vector<Graph *> *controlGraphsArray{};
    std::vector<SuperGraphProp *> *graphProps{};
    GenSGMode genSupergraphs;

 public:
    hvec_map<cstring, DepEdgeMap> foundDepEdges;
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_IDE_PASS_H_ */
