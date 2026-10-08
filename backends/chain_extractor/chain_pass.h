#ifndef BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_
#define BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_

#include "backends/ifds/domain.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/ide_pass.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "ir/ir.h"

namespace P4::P4ChainExtractor {

using namespace ::P4::IFDS;

using Graph = Graphs::Graph;

/// Dependency maps of one control graph, in the shapes the DependencyGraphs pipeline consumes.
struct GraphChainMaps {
    /// Action param -> stateful-object sink (src -> dst), and its register-read seeds.
    IDEPass::DepEdgeMap a2s;
    std::vector<TabVertex> aStateVars;
    IDEPass::DepEdgeMap aStateVarMap;
    /// Stateful-object sink -> header (dst -> src), and its register-read seeds.
    IDEPass::DepEdgeMap h2s;
    std::vector<TabVertex> hStateVars;
    IDEPass::DepEdgeMap hStateVarMap;
    /// Register read -> sink, restricted to the register reads of each family.
    IDEPass::DepEdgeMap a2s2kv;  // key and header/port sinks (A2S2V)
    IDEPass::DepEdgeMap a2s2k;   // key sinks
    IDEPass::DepEdgeMap a2s2c;   // condition sinks
    IDEPass::DepEdgeMap h2s2k;   // key sinks
    IDEPass::DepEdgeMap h2s2v;   // header/port sinks
    IDEPass::DepEdgeMap h2s2c;   // condition sinks
};

/// Computes every chain relation of a control graph from IDE solves over three domains:
/// Act (action params), Hdr (headers, parser-derived metadata, ingress port) and SO (register
/// reads). By default one solve covers all domains and the implicit match-key -> action-defVar
/// edges carry every domain. With legacyFlow, {Act, SO} and {Hdr} are solved separately and
/// only Hdr sees the implicit edges, which reproduces p4c_state_dependency exactly.
class FindChains : public IDEPass {
 public:
    FindChains(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
               std::vector<Graph *> *controlGraphsArray,
               std::vector<SuperGraphProp *> *graphProps, GenSGMode genSupergraphs,
               bool needAct, bool needHdr, bool legacyFlow)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs),
          needAct(needAct), needHdr(needHdr), legacyFlow(legacyFlow) {}

    Visitor::profile_t init_apply(const IR::Node *) override;

    /// Maps of @graphName; empty when the graph has no chain.
    const GraphChainMaps &maps(cstring graphName) const;
    /// Maps of @graph (by its graph_name); empty when the graph has no chain.
    const GraphChainMaps &maps(const Graph *graph) const;

 protected:
    void analyze_control_graph(Tabulation *tab) override;
    /// Seeds of every domain, in domain order (used when drawing edge functions).
    std::vector<TabVertex> get_seeds(Tabulation *tab) override;

 private:
    class ActDomain;
    class HdrDomain;
    class SODomain;

    std::vector<TabVertex> header_seeds(Tabulation *tab);
    std::vector<TabVertex> register_read_seeds(Tabulation *tab);
    /// Append @tv to @seeds unless @seen already holds it.
    static void add_seed(const TabVertex &tv, hvec_set<TabVertex, TabVertexHash> &seen,
                         std::vector<TabVertex> &seeds);
    /// Implicit flow of a table: its match-key vars -> every global var its actions define.
    void add_implicit_table_flow(Tabulation *tab, FlowOverlay &overlay);
    /// Register-read -> sink maps of one family, in the family's seed order.
    void collect_so_sinks(Tabulation *tab, const DomainSet &domains, size_t so,
                          const std::vector<TabVertex> &familySeeds,
                          IDEPass::DepEdgeMap *key, IDEPass::DepEdgeMap *hdr,
                          IDEPass::DepEdgeMap *keyHdr, IDEPass::DepEdgeMap *cond);
    void print_counters(Tabulation *tab);

    bool needAct;
    bool needHdr;
    bool legacyFlow;
    hvec_map<cstring, GraphChainMaps> graphMaps;
};

}  // namespace P4::P4ChainExtractor

#endif /* BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_ */
