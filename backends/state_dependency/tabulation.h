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

    using TabEdge = std::pair<TabVertex, TabVertex>;

    explicit Tabulation(Graph *g, SuperGraphProp *sgProp)
        : g(g), sgProp(sgProp) {
            rootTv = TabVertex{sgProp->rootVar, Graphs::globalNode};
        }

    Graphs::vertex_t get_vertex_id(TabVertex &tb) {
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
            auto edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR)
                return TabVertex{boost::source(*ei, *g), vinfo.node};
        }

        // Unreachable
        return TabVertex{};
    }

    cstring dump_tab_vertex(TabVertex &a) const {
        std::stringstream logstr;
        if (a.node == sgProp->rootVar)
            logstr << "ROOT:";
        else
            logstr << a.node << ":";
        logstr << a.var;
        return cstring(logstr);
    }

 public:
    Graph *g;
    SuperGraphProp *sgProp;
    TabVertex rootTv;

    void init();
    void forward_tabulate();
    void propagate(TabVertex a, TabVertex b);
    std::vector<TabVertex> &get_successors(TabVertex &tb,
            std::vector<TabVertex> &succ);

 private:
    //std::vector<TabVertex *> get_successors(TabVertex *d);

    hvec_set<TabEdge> pathEdge;
    std::queue<TabEdge> workList;
    hvec_set<TabEdge> summaryEdge;
};

}  // namespace P4::P4StateDependency

namespace std {
template <>
struct hash<P4::P4StateDependency::Tabulation::TabVertex> {
    std::size_t operator()(const P4::P4StateDependency::Tabulation::TabVertex &t) const { return t.hash(); }
};

}  // namespace std

#endif /* BACKENDS_STATE_DEPENDENCY_TABULATION_H_ */
