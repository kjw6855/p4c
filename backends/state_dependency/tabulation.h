#ifndef BACKENDS_STATE_DEPENDENCY_TABULATION_H_
#define BACKENDS_STATE_DEPENDENCY_TABULATION_H_

#include <queue>

#include "graphs.h"
#include "supergraphs.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

using Graph = Graphs::Graph;

class Tabulation : public Graphs {
 public:

    using FuncMap = hvec_map<TabEdge, EdgeFuncHolder>;

    struct FuncMapHelper {
     public:
        void add_func(const TabEdge &te, EdgeFuncHolder fn) {
            funcMap[te] = fn;
            funcSecondKeyMap[te.second].insert(te);
            funcKeyMap[{te.first.node, te.second.node}].insert(te);
        }

        FuncMap get_func_by_nodes(const Graphs::vertex_t &a, const Graphs::vertex_t &b) {
            FuncMap foundFunc;
            auto fkit = funcKeyMap.find({a, b});
            if (fkit == funcKeyMap.end()) return foundFunc;
            for (auto funcKey : fkit->second)
                foundFunc[funcKey] = funcMap[funcKey];
            return foundFunc;
        }

        FuncMap get_func_by_second(const TabVertex &tb) {
            FuncMap foundFunc;
            auto fskit = funcSecondKeyMap.find(tb);
            if (fskit == funcSecondKeyMap.end()) return foundFunc;
            for (auto funcKey : fskit->second)
                foundFunc[funcKey] = funcMap[funcKey];
            return foundFunc;
        }

        EdgeFuncHolder &operator[](const TabEdge &k) {
            return funcMap[k];
        }

        EdgeFuncHolder &operator[](TabEdge &&k) {
            return funcMap[k];
        }

     private:
        FuncMap funcMap;
        hvec_map<TabVertex, hvec_set<TabEdge>, TabVertexHash> funcSecondKeyMap;
        hvec_map<std::pair<Graphs::vertex_t, Graphs::vertex_t>, hvec_set<TabEdge>> funcKeyMap;
    };

    Graphs::vertex_t get_vertex_id(const TabVertex &tb) {
        if (tb.node == sgProp->rootVar) return tb.node;
        auto ninfo = (*g)[tb.node];
        BUG_CHECK(!hasFlag(ninfo.flags, VertexFlags::VARIABLE),
                  "TabVertex has wrong node Id %1%", tb.node);

        auto idx = sgProp->progVarInfo.get_var_index(tb.var,
                sgProp->procOf[tb.node]);
        return sgProp->progVarInfo[tb.node][idx];
    }

    TabVertex get_tab_vertex(Graphs::vertex_t v) {
        if (v == sgProp->rootVar) return rootTv;
        auto vinfo = (*g)[v];
        BUG_CHECK(hasFlag(vinfo.flags, VertexFlags::VARIABLE),
                "%1% is not variable", v);

        for (auto [ei, ei_end] = boost::in_edges(v, *g); ei != ei_end; ++ei) {
            auto &edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR)
                return TabVertex{boost::source(*ei, *g), vinfo.node};
        }

        // Unreachable
        return TabVertex{};
    }

    cstring dump_tab_vertex(const TabVertex &a) {
        std::stringstream logstr;
        if (a.node == sgProp->rootVar)
            logstr << "ROOT:";
        else {
            auto &ainfo = (*g)[a.node];
            logstr << ainfo.name << "(";
            logstr << a.node << "):";
        }
        auto tvIt = get_vertex_id(a);
        auto tvInfo = (*g)[tvIt];
        logstr << tvInfo.name;
        return cstring(logstr);
    }

    cstring dump_tab_edge(const TabEdge &a) {
        std::stringstream logstr;
        logstr << dump_tab_vertex(a.first) << "->";
        logstr << dump_tab_vertex(a.second);
        return cstring(logstr);
    }

    explicit Tabulation(Graph *g, SuperGraphProp *sgProp)
        : g(g), sgProp(sgProp) {
            rootTv = TabVertex{sgProp->rootVar, Graphs::globalNode};
        }

    std::vector<TabVertex> get_incoming(const TabVertex &a) {
        auto iit = incomingList.find(a);
        if (iit == incomingList.end()) return {};
        return iit->second;
    }

    std::vector<TabVertex> get_end_summary(const TabVertex &a) {
        auto esit = endSummary.find(a);
        if (esit == endSummary.end()) return {};
        return esit->second;
    }

 private:
    void propagate_ifds(TabVertex a, TabVertex b);
    void propagate_ide(TabVertex a, TabVertex b, EdgeFuncHolder fn);
    void propagate_value_ide(TabVertex tv, VarBitSet val);
    std::vector<TabVertex> &get_successors(TabVertex &tb,
            std::vector<TabVertex> &succ);
    EdgeFuncHolder get_edge_func(TabVertex &a, TabVertex &b);
    VarBitSet may_meet_value(VarBitSet a, VarBitSet b);
    std::vector<TabVertex> get_return_val(const TabVertex &exitTv,
            const TabVertex &callerTv);

 public:
    Graph *g;
    SuperGraphProp *sgProp;
    TabVertex rootTv;

    // Initialization for supergraph's EdgeFunc
    bool init_edge_func(const std::vector<TabVertex> &tvList);
    std::vector<TabVertex> get_target_vars(const VarBitSet &bitmap);
    std::optional<size_t> get_target_var_id(const TabVertex &tv);
    void clear_edge_func();

    void init_ifds();
    void init_ide();
    void forward_tabulate_ifds();
    void forward_tabulate_ide();
    void forward_tabulate_on_demand_ide();
    void compute_values_ide();
    void find_path(std::vector<TabVertex> &tvs);
    void dump_result();
    bool sanity_check_ide();


    hvec_set<TabEdge> pathEdge;
    hvec_set<TabEdge> summaryEdge;
    hvec_map<Graphs::vertex_t, std::vector<const IR::Node *>> reachableVars;
    FuncMapHelper jumpFunc;
    FuncMapHelper summaryFunc;
    hvec_map<TabVertex, VarBitSet, TabVertexHash> valueMap;

    // Used for EdgeFunc
    size_t varBitSetSize;
    std::vector<TabVertex> targetVars;
    hvec_map<TabVertex, size_t, TabVertexHash> targetVarIdMap;
    EdgeFuncHolder topFunc;
    VarBitSet topEnvValue;

 private:
    std::queue<TabEdge> workList;
    std::queue<TabVertex> nodeWorkList;
    hvec_map<TabVertex, std::vector<TabVertex>, TabVertexHash> incomingList;
    hvec_map<TabVertex, std::vector<TabVertex>, TabVertexHash> endSummary;
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_TABULATION_H_ */
