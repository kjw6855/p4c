#ifndef BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_
#define BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_

#include <map>
#include <set>
#include <vector>

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
    /// RegisterAction body statements an action param reaches through a global (e.g. `v = ig_md.v`)
    /// -> the RETURN values of the body's call sites carrying that param. Only CALL/RETURN carry the
    /// register, so DependencyGraphs::add_body_writes_from_map draws these calls (write_to).
    IDEPass::DepEdgeMap aBodyWrites;
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
    /// Register read -> sink that sets the packet's output (port, drop, multicast group):
    /// O = the output variables themselves (data flow); OutC = conditions guarding an output write.
    IDEPass::DepEdgeMap a2s2o;
    IDEPass::DepEdgeMap a2s2outc;
    IDEPass::DepEdgeMap h2s2o;
    IDEPass::DepEdgeMap h2s2outc;
    /// OutK = match keys of tables where some action writes an output.
    IDEPass::DepEdgeMap a2s2outk;
    IDEPass::DepEdgeMap h2s2outk;

    /// Hop annotation of the output chains (default flow only), from the ActSel domain:
    /// sinkSelect: per OutK/OutC sink fact, the actions whose effect reaches it, by table;
    /// keyReads:   per table, the registers (SOChain::soName) whose reads reach its match key;
    /// outputActions: per output-writing table, its actions that write an output;
    /// tableOrder: CFG position of each table, to order the hops.
    using SinkFact = std::pair<Graphs::vertex_t, const IR::Node *>;
    std::map<SinkFact, std::map<cstring, std::set<cstring>>> sinkSelect;
    std::map<cstring, std::set<cstring>> keyReads;
    std::map<cstring, std::vector<cstring>> outputActions;
    std::map<cstring, size_t> tableOrder;
};

/// Sink maps filled by one collect pass; null entries are skipped.
struct SinkMaps {
    IDEPass::DepEdgeMap *key = nullptr;     // table match keys
    IDEPass::DepEdgeMap *hdr = nullptr;     // header / egress port at the end of the control
    IDEPass::DepEdgeMap *keyHdr = nullptr;  // both of the above in one map
    IDEPass::DepEdgeMap *cond = nullptr;    // if / switch conditions
    IDEPass::DepEdgeMap *out = nullptr;     // output variables at the end of the control
    IDEPass::DepEdgeMap *outCond = nullptr; // conditions whose branches write an output
    IDEPass::DepEdgeMap *outKey = nullptr;  // keys of tables whose actions write an output
};

/// True if @expr is (a slice of) packet metadata deciding the packet's output: the egress port,
/// the drop control or the multicast group of v1model or tna.
bool is_output_lvalue(const IR::Expression *expr, P4::ReferenceMap *refMap);

/// Finds whether code writes an output (is_output_lvalue or mark_to_drop), following action
/// calls and table applies into the action bodies.
class OutputWriteFinder : public Inspector {
 public:
    OutputWriteFinder(P4::ReferenceMap *refMap, P4::TypeMap *typeMap)
        : refMap(refMap), typeMap(typeMap) {}
    bool writes(const IR::Node *node);
    /// Whether some action of @table (the default action included) writes an output.
    bool table_writes(const IR::P4Table *table);

    bool preorder(const IR::AssignmentStatement *assign) override;
    bool preorder(const IR::MethodCallExpression *call) override;

 private:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    bool found = false;
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
               bool needAct, bool needHdr, bool legacyFlow, bool needHops = false)
        : IDEPass(refMap, typeMap, controlGraphsArray, graphProps, genSupergraphs),
          needAct(needAct), needHdr(needHdr), legacyFlow(legacyFlow),
          needHops(needHops && !legacyFlow) {}

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
    class ActSelDomain;

    std::vector<TabVertex> header_seeds(Tabulation *tab);
    std::vector<TabVertex> register_read_seeds(Tabulation *tab);
    /// Append @tv to @seeds unless @seen already holds it.
    static void add_seed(const TabVertex &tv, hvec_set<TabVertex, TabVertexHash> &seen,
                         std::vector<TabVertex> &seeds);
    /// Every global defined inside an action body (plus the egress port at a drop statement).
    std::vector<TabVertex> action_def_seeds(Tabulation *tab);
    /// Fill GraphChainMaps::aBodyWrites from the A first hop (@act: the Act domain index).
    void collect_body_writes(Tabulation *tab, const DomainSet &domains, size_t act,
                             GraphChainMaps &m);
    /// Append @vv to the RETURN values @rets of a body write unless present (keeps call-site order).
    static void add_body_write(std::vector<Graphs::VarVertex> &rets, Graphs::VarVertex vv);
    /// The tables (control-plane names) calling the action procedure of @seed, and the action,
    /// computed once per action procedure into @cache.
    const std::pair<std::vector<cstring>, cstring> &cached_action_of(
        Tabulation *tab, const hvec_map<cstring, cstring> &tableName,
        hvec_map<cstring, std::pair<std::vector<cstring>, cstring>> &cache, const TabVertex &seed);
    /// Fill the hop-annotation maps of @m from a solve that registered the ActSel domain.
    void collect_hops(Tabulation *tab, const DomainSet &domains, size_t so, size_t actSel,
                      GraphChainMaps &m);
    /// Implicit flow of a table: its match-key vars -> every global var its actions define.
    void add_implicit_table_flow(Tabulation *tab, FlowOverlay &overlay);
    /// Register-read -> sink maps of one family, in the family's seed order.
    void collect_so_sinks(Tabulation *tab, const DomainSet &domains, size_t so,
                          const std::vector<TabVertex> &familySeeds, const SinkMaps &sinks);
    /// Whether the if/switch of a condition vertex writes an output in one of its branches.
    bool guards_output(const Graphs::Vertex &vinfo);
    /// Whether a KEY vertex belongs to a table where some action writes an output.
    bool key_decides_output(Graph *g, Graphs::vertex_t keyVertex);
    void print_counters(Tabulation *tab);

    bool needAct;
    bool needHdr;
    bool legacyFlow;
    bool needHops;
    hvec_map<cstring, GraphChainMaps> graphMaps;
    hvec_map<const IR::Node *, bool> guardsOutput;
    hvec_map<const IR::Node *, bool> tableWritesOutput;
};

}  // namespace P4::P4ChainExtractor

#endif /* BACKENDS_CHAIN_EXTRACTOR_CHAIN_PASS_H_ */
