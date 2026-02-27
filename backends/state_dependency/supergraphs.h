#ifndef BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_

#include <optional>

#include "frontends/common/resolveReferences/resolveReferences.h"
#include "graphs.h"
#include "ir/ir.h"
#include "lib/hvec_map.h"
#include "lib/hvec_set.h"

namespace P4::P4StateDependency {

class SuperGraphs : public Graphs,
                    public Inspector,
                    public P4::ResolutionContext {
 public:
    SuperGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars,
                std::vector<Graph *> *controlGraphsArray);

    void gen_supergraph(Graph *g_);
    void init_all_variables();
    void setup_root_vars();
    void gen_ifds_edge(Graphs::vertex_t src, Graphs::vertex_t dst);
    bool preorder(const IR::PackageBlock *block);

    //bool preorder(const IR::PackageBlock *block) override;
 protected:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    std::vector<Graph *> *controlGraphsArray{};
    hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars;

    std::size_t varNum;
    std::vector<const IR::Node *> variableList;
    hvec_map<const IR::Node *, std::size_t> varIndexMap;
    hvec_map<Graphs::vertex_t, std::vector<Graphs::vertex_t>> globalVariables;
};

}  // namespace P4::P4StateDependency

#endif  /* BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_ */
