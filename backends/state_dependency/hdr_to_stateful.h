#ifndef BACKENDS_STATE_DEPENDENCY_HDR_TO_STATEFUL_H_
#define BACKENDS_STATE_DEPENDENCY_HDR_TO_STATEFUL_H_

#include "backends/ifds/ide_pass.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using namespace ::P4::IFDS;

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
    std::vector<TabVertex> get_seeds(Tabulation *tab) override;
    // Seed edges from the zero fact, and match key -> action defVar edges
    void add_flow_functions(Tabulation *tab, const std::vector<TabVertex> &seeds,
                            FlowOverlay &overlay) override;
    void analyze_control_graph(Tabulation *tab) override;
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
