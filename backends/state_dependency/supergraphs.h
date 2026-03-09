#ifndef BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_

#include <optional>

#include "graphs.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

// <node, var> form of supergraph
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

struct TabVertexHash {
    size_t operator()(const TabVertex &t) const noexcept {
        return t.hash();
    }
};

using TabEdge = std::pair<TabVertex, TabVertex>;

class SuperGraphProp {
 public:
    std::optional<size_t> get_action_param_id(const TabVertex &tv) {
        auto actionParamIdMapIt = actionParamIdMap.find(tv);
        return actionParamIdMapIt == actionParamIdMap.end() ?
            std::nullopt : std::optional{actionParamIdMapIt->second};
    }

    std::pair<Graphs::vertex_t, Graphs::vertex_t> get_call_map(const Graphs::vertex_t &a) {
        auto callMapIt = callMap.find(a);
        if (callMapIt == callMap.end()) BUG("No callMap for %2%", a);
        return callMapIt->second;
    }

    Graphs::vertex_t rootVar;
    std::size_t varNum;
    std::vector<const IR::Node *> variableList;
    hvec_map<const IR::Node *, std::size_t> varIndexMap;
    hvec_map<Graphs::vertex_t, std::vector<Graphs::vertex_t>> globalVariables;
    Graphs::ProcOf procOf;
    Graphs::CallMap callMap;
    Graphs::ProcCallers procCallerMap;
    hvec_map<cstring, std::vector<Graphs::vertex_t>> caller;
    hvec_map<const IR::Node *, Graphs::vertex_t> defBy;
    hvec_map<cstring, Graphs::vertex_t> srcOf;
    std::vector<TabVertex> actionParams;
    hvec_map<TabVertex, size_t, TabVertexHash> actionParamIdMap;
    std::size_t topEnvValue;
    EdgeFuncHolder topFunc;

    std::vector<TabVertex> get_action_params(size_t bitmap);
};

class SuperGraphs : public Graphs {
 public:
    SuperGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                std::vector<Graph *> *controlGraphsArray,
                hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars,
                hvec_map<cstring, Graphs::ProcOf> *procOfs,
                hvec_map<cstring, Graphs::CallMap> *callMaps,
                hvec_map<cstring, Graphs::ProcCallers> *procCallerMaps)
    : refMap(refMap),
      typeMap(typeMap),
      controlGraphsArray(controlGraphsArray),
      graphVars(graphVars),
      procOfs(procOfs),
      callMaps(callMaps),
      procCallerMaps(procCallerMaps) {}


    void gen_supergraphs();

 private:
    void gen_supergraph(Graph *g_, SuperGraphProp *sgProp);
    void create_var_vertices(const cstring &);
    void create_root_var_vertex();
    void gen_ifds_edge(Graphs::vertex_t src, Graphs::vertex_t dst);

 protected:
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    std::vector<Graph *> *controlGraphsArray{};
    hvec_map<cstring, hvec_set<const IR::Node *>> *graphVars;
    hvec_map<cstring, Graphs::ProcOf> *procOfs;
    hvec_map<cstring, Graphs::CallMap> *callMaps;
    hvec_map<cstring, Graphs::ProcCallers> *procCallerMaps;

    SuperGraphProp *curProp{};

 public:
    std::vector<SuperGraphProp*> graphProps;

};

}  // namespace P4::P4StateDependency

namespace std {
template <>
struct hash<P4::P4StateDependency::TabVertex> {
    std::size_t operator()(const P4::P4StateDependency::TabVertex &t) const { return t.hash(); }
};

}  // namespace std

#endif  /* BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_ */
