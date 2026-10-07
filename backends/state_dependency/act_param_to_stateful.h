#ifndef BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_

#include "backends/ifds/ide_pass.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using namespace ::P4::IFDS;

using Graph = Graphs::Graph;

class FindActParamToStateful : public IDEPass {

 public:
    explicit FindActParamToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 protected:
    std::vector<TabVertex> get_seeds(Tabulation *tab) override;
    void analyze_control_graph(Tabulation *tab) override;
};

class ActParamToStateful : public PassManager {
 public:
    explicit ActParamToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs) {
        ptsPass = new FindActParamToStateful(refMap, typeMap,
                    controlGraphsArray,
                    graphProps, genSupergraphs);
        passes.push_back(ptsPass);
    }

    IDEPass::DepEdgeMap getFoundDepEdges(const cstring &graphName) {
        if (ptsPass->foundDepEdges.find(graphName) ==
                ptsPass->foundDepEdges.end())
            return IDEPass::DepEdgeMap{};
        return ptsPass->foundDepEdges[graphName];
    }

    hvec_map<cstring, IDEPass::DepEdgeMap> *getAllFoundDepEdges() {
        return &ptsPass->foundDepEdges;
    }

    void set_edge_func() {
        ptsPass->set_edge_func();
    }
 protected:
    FindActParamToStateful *ptsPass{};
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_ACT_PARAM_TO_STATEFUL_H_ */
