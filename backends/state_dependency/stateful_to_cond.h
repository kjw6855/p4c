#ifndef BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_COND_H_
#define BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_COND_H_

#include "ide_pass.h"
#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

/// IFDS/IDE pass that finds dataflow from stateful objects into the variables
/// of condition-check vertices (VertexFlags::CONDITION for if-statements and
/// VertexFlags::SWITCH for switch-expressions).
///
/// Used as phase S2C (Stateful → Condition) in the full analysis pipeline.
/// Feeds from a preceding A2S2V or H2S2V pass via prevDepEdgeMaps so the
/// printed output can show the full dependency chain.
class FindStatefulToCond : public IDEPass {

 public:
    explicit FindStatefulToCond(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
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

 protected:
    hvec_map<cstring, std::vector<TabVertex>> *stateVars{};
    hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps{};
    cstring analysisType;
};

class StatefulToCond : public PassManager {
 public:
    explicit StatefulToCond(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs,
            hvec_map<cstring, std::vector<TabVertex>> *stateVars,
            hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps,
            cstring analysisType) {
        stdPass = new FindStatefulToCond(refMap, typeMap,
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
    FindStatefulToCond *stdPass{};
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_COND_H_ */
