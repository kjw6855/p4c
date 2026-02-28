#ifndef BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_

#include <optional>

#include "graphs.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

class SuperGraphProp {
 public:
    Graphs::vertex_t rootVar;
    std::size_t varNum;
    std::vector<const IR::Node *> variableList;
    hvec_map<const IR::Node *, std::size_t> varIndexMap;
    hvec_map<Graphs::vertex_t, std::vector<Graphs::vertex_t>> globalVariables;
};

class SuperGraphs : public Graphs {
 public:
    SuperGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars,
                std::vector<Graph *> *controlGraphsArray);

    void gen_supergraphs();

 private:
    void gen_supergraph(Graph *g_, SuperGraphProp *sgProp);
    void create_var_vertices(const cstring &);
    void create_root_var_vertex();
    void gen_ifds_edge(Graphs::vertex_t src, Graphs::vertex_t dst);

 protected:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars;
    std::vector<Graph *> *controlGraphsArray{};

    SuperGraphProp *curProp{};

 public:
    std::vector<SuperGraphProp> graphProps;

};

}  // namespace P4::P4StateDependency

#endif  /* BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_ */
