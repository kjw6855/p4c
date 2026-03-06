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
    struct TabVertex {
        Graphs::vertex_t node;
        const IR::Node *var;
        mutable size_t computedHash = 0;

        /*
        TabVertex() = default;
        TabVertex(Graphs::vertex_t node, const IR::Node *var)
            : node(node), var(var) {}
            */
        bool operator==(const TabVertex &a) const {
            if (node != a.node) return false;
            return var == a.var;
        }

        std::size_t hash() const {
            if (!computedHash) {
                computedHash = Util::Hash{}(node, var);
            }
            return computedHash;
        }
    };

    struct TabVertexHash {
        size_t operator()(const TabVertex &t) const noexcept {
            return t.hash();
        }
    };

    using TabEdge = std::pair<TabVertex, TabVertex>;

    explicit Tabulation(Graph *g, SuperGraphProp *sgProp)
        : g(g), sgProp(sgProp) {
            rootTv = TabVertex{sgProp->rootVar, Graphs::globalNode};
        }

    Graphs::vertex_t get_vertex_id(const TabVertex &tb) {
        if (tb.node == sgProp->rootVar) return tb.node;
        auto ninfo = (*g)[tb.node];
        BUG_CHECK(!hasFlag(ninfo.flags, VertexFlags::VARIABLE),
                  "TabVertex has wrong node Id %1%", tb.node);

        auto idx = sgProp->varIndexMap[tb.var];
        return sgProp->globalVariables[tb.node][idx];
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

 private:
    void propagate_ifds(TabVertex a, TabVertex b);
    void propagate_ide(TabVertex a, TabVertex b, EdgeFuncHolder fn);
    void propagate_value_ide(TabVertex tv, size_t val);
    std::vector<TabVertex> &get_successors(TabVertex &tb,
            std::vector<TabVertex> &succ);
    EdgeFuncHolder get_edge_func(TabVertex &a, TabVertex &b);
    size_t may_meet_value(size_t a, size_t b);

 public:
    Graph *g;
    SuperGraphProp *sgProp;
    TabVertex rootTv;

    void init_ifds();
    void init_ide();
    void forward_tabulate_ifds();
    void forward_tabulate_ide();
    void compute_values_ide();
    void find_path(std::vector<TabVertex> &tvs);
    void dump_result();

    hvec_set<TabEdge> pathEdge;
    hvec_set<TabEdge> summaryEdge;
    hvec_map<Graphs::vertex_t, std::vector<const IR::Node *>> reachableVars;
    hvec_map<TabEdge, EdgeFuncHolder> jumpFunc;
    hvec_map<TabEdge, EdgeFuncHolder> summaryFunc;
    hvec_map<TabVertex, size_t, TabVertexHash> valueMap;

 private:
    std::queue<TabEdge> workList;
    std::queue<TabVertex> nodeWorkList;
};

}  // namespace P4::P4StateDependency

namespace std {
template <>
struct hash<P4::P4StateDependency::Tabulation::TabVertex> {
    std::size_t operator()(const P4::P4StateDependency::Tabulation::TabVertex &t) const { return t.hash(); }
};

}  // namespace std

#endif /* BACKENDS_STATE_DEPENDENCY_TABULATION_H_ */
