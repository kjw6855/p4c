#ifndef BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_KEY_H_
#define BACKENDS_STATE_DEPENDENCY_STATEFUL_TO_KEY_H_

#include "ide_pass.h"
#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

/// Which sink kind this pass harvests. Each sink type gets its own dependency graph
/// (H2S2K for keys, H2S2V for header/port values) so that a field which is both a table
/// key and a written header is not double-counted into a single graph's SOChains.
enum class KeySinkMode {
    KEY_ONLY,        ///< Harvest only KEY-flagged sink vertices (table match keys).
    HEADER_ONLY,     ///< Harvest only EXIT/header sink vertices (output packet fields/ports).
    KEY_AND_HEADER,  ///< Harvest both (legacy behavior for A2S2V and the S2V viz checker).
};

class FindStatefulToKey : public IDEPass {

 public:
    explicit FindStatefulToKey(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs,
            hvec_map<cstring, std::vector<TabVertex>> *stateVars,
            hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps,
            cstring analysisType,
            KeySinkMode sinkMode)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs),
          stateVars(stateVars), prevDepEdgeMaps(prevDepEdgeMaps), analysisType(analysisType),
          sinkMode(sinkMode) {}
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
    KeySinkMode sinkMode;
};

class StatefulToKey : public PassManager {
 public:
    explicit StatefulToKey(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            std::vector<Graph *> *controlGraphsArray,
            std::vector<SuperGraphProp *> *graphProps,
            GenSGMode genSupergraphs,
            hvec_map<cstring, std::vector<TabVertex>> *stateVars,
            hvec_map<cstring, IDEPass::DepEdgeMap> *prevDepEdgeMaps,
            cstring analysisType,
            KeySinkMode sinkMode) {
        stdPass = new FindStatefulToKey(refMap, typeMap,
                    controlGraphsArray, graphProps,
                    genSupergraphs, stateVars, prevDepEdgeMaps, analysisType,
                    sinkMode);
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
