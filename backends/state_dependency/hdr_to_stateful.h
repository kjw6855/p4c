#ifndef BACKENDS_STATE_DEPENDENCY_HDR_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_HDR_TO_STATEFUL_H_

#include "ide_pass.h"
#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class FindHdrToStateful : public IDEPass {

 public:
    explicit FindHdrToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs) {}
    Visitor::profile_t init_apply(const IR::Node *) override;

 protected:
    void set_edge_func_in_graph(Tabulation *tab) override;
    void analyze_control_graph(Tabulation *tab) override;

    std::vector<std::pair<Graphs::vertex_t, Graphs::vertex_t>> tempEdges{};
};

class HdrToStateful : public PassManager {
 public:
    explicit HdrToStateful(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs) {
        htsPass = new FindHdrToStateful(refMap, typeMap,
                    controlGraphsArray,
                    graphProps, genSupergraphs);
        passes.push_back(htsPass);
    }

    IDEPass::DepEdgeMap getFoundDepEdges(const cstring &graphName) {
        if (htsPass->foundDepEdges.find(graphName) ==
                htsPass->foundDepEdges.end())
            return IDEPass::DepEdgeMap{};
        return htsPass->foundDepEdges[graphName];
    }

    hvec_map<cstring, IDEPass::DepEdgeMap> *getAllFoundDepEdges() {
        return &htsPass->foundDepEdges;
    }

    void set_edge_func() {
        htsPass->set_edge_func();
    }
 protected:
    FindHdrToStateful *htsPass{};
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_HDR_TO_STATEFUL_H_ */
