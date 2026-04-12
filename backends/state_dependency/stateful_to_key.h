#ifndef BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_KEY_H_
#define BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_KEY_H_

#include "ide_pass.h"
#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class FindStatefulToKey : public IDEPass {

 public:
    explicit FindStatefulToKey(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs,
            hvec_map<cstring, std::vector<TabVertex>> *stateVars,
            hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps,
            cstring analysisType)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs),
          stateVars(stateVars), prevDepEdgeMaps(prevDepEdgeMaps), analysisType(analysisType) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 protected:
    void set_edge_func_in_graph(Tabulation *tab) override;
    void analyze_control_graph(Tabulation *tab) override;

 private:
    std::vector<const IR::Node *> find_ret_vars(Tabulation *tab, Graphs::vertex_t ret_v);

 protected:
    hvec_map<cstring, std::vector<TabVertex>> *stateVars{};
    hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps{};
    cstring analysisType;
};

class StatefulToKey : public PassManager {
 public:
    explicit StatefulToKey(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs,
            hvec_map<cstring, std::vector<TabVertex>> *stateVars,
            hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps,
            cstring analysisType) {
        stdPass = new FindStatefulToKey(refMap, typeMap,
                    controlGraphsArray, graphProps,
                    genSupergraphs, stateVars, prevDepEdgeMaps, analysisType);
        passes.push_back(stdPass);
    }

    IDEPass::DepEdgeMap getFoundDepEdges(const cstring &graphName) {
        if (stdPass->foundDepEdges.find(graphName) ==
                stdPass->foundDepEdges.end())
            return IDEPass::DepEdgeMap{};
        return stdPass->foundDepEdges[graphName];
    }

    void set_edge_func() {
        stdPass->set_edge_func();
    }
 protected:
    FindStatefulToKey *stdPass{};
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_KEY_H_ */
