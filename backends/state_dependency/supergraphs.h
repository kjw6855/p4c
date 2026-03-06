#ifndef BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_

#include <optional>

#include "graphs.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

class SuperGraphProp {
 public:

    std::optional<size_t> get_action_id(Graphs::vertex_t v) {
        auto actionIdMapIt = actionIdMap.find(v);
        return actionIdMapIt == actionIdMap.end() ?
            std::nullopt : std::optional{actionIdMapIt->second};
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
    std::vector<Graphs::vertex_t> actions;
    hvec_map<Graphs::vertex_t, size_t> actionIdMap;
    hvec_map<cstring, Graphs::vertex_t> srcOf;
    std::size_t topEnvValue;

    std::vector<Graphs::vertex_t> get_action_vertices(size_t bitmap);
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

#endif  /* BACKENDS_STATE_DEPENDENCY_SUPERGRAPHS_H_ */
