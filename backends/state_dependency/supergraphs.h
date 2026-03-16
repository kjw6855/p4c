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

// Define variable information like mapping
// Override Graphs to add vertex
struct VarInfo {
    std::size_t varNum = 0;
    std::vector<const IR::Node *> variableList; // idx->var
    hvec_map<const IR::Node *, std::size_t> varIndexMap; // var->idx

    void add_var(const IR::Node *n) {
        variableList.push_back(n);
        varIndexMap[n] = varNum++;
    }
};
/*
 * Manage global + local variables and their vertices
 * - add_var() puts variables in either global or local
 * - get_all_vars() returns all accessible variables in procedure
 * - get_var_index() returns index of accesible variables in procedure
 * - push_var_vertex_id() stores vid in the order of accessible variables
 */
struct ProgramVarInfo {
    VarInfo globalVars;
    hvec_map<cstring, VarInfo> localVars;
    hvec_map<cstring, std::vector<const IR::Node *>> cachedProcVarInfo;
    hvec_map<Graphs::vertex_t, std::vector<Graphs::vertex_t>> varVertices;

    void add_var(const IR::Node *n,
            std::optional<cstring> procName=std::nullopt) {
        if (!procName.has_value())
            globalVars.add_var(n);
        else
            localVars[procName.value()].add_var(n);
    }

    const std::vector<const IR::Node *> &get_all_vars(std::optional<cstring> procName=std::nullopt) {
        if (!procName.has_value()) return globalVars.variableList;

        // Skip if no local vars
        auto lvit = localVars.find(procName.value());
        if (lvit == localVars.end()) return globalVars.variableList;
        if (lvit->second.varNum == 0) return globalVars.variableList;

        // Return if there are cached vars
        auto cpviit = cachedProcVarInfo.find(procName.value());
        if (cpviit != cachedProcVarInfo.end()) return cpviit->second;

        // Put global + local into cache
        auto &procVarInfo = cachedProcVarInfo[procName.value()];
        procVarInfo.insert(procVarInfo.end(), globalVars.variableList.begin(),
                globalVars.variableList.end());
        procVarInfo.insert(procVarInfo.end(), lvit->second.variableList.begin(),
                lvit->second.variableList.end());

        return procVarInfo;
    }

    std::size_t get_var_index(const IR::Node *n,
            std::optional<cstring> procName=std::nullopt) {
        if (!procName.has_value()) return globalVars.varIndexMap[n];
        auto &allVars = get_all_vars(procName);
        auto vit = std::find(allVars.begin(), allVars.end(), n);
        if (vit != allVars.end())
            return std::distance(allVars.begin(), vit);

        BUG("Can't find variable index for %1%", n);
        return 0;
    }

    std::size_t get_var_num(std::optional<cstring> procName=std::nullopt) {
        if (!procName.has_value()) return globalVars.varNum;
        auto lvit = localVars.find(procName.value());
        if (lvit == localVars.end()) return globalVars.varNum;
        return globalVars.varNum + lvit->second.varNum;
    }

    void push_var_vertex_id(Graphs::vertex_t nid, Graphs::vertex_t vid) {
        varVertices[nid].push_back(vid);
    }

    std::vector<Graphs::vertex_t> &operator[](Graphs::vertex_t nid) {
        BUG_CHECK(varVertices.size() > 0, "Create varVertices first");
        return varVertices[nid];
    }

    inline bool is_local(size_t idx) {
        return (idx >= globalVars.varNum);
    }

    bool is_local(const IR::Node *n, cstring procName) {
        size_t idx = get_var_index(n, procName);
        return is_local(idx);
    }
};

class SuperGraphProp {
 public:
    std::pair<Graphs::vertex_t, Graphs::vertex_t> get_call_map(const Graphs::vertex_t &a) {
        auto callMapIt = callMap.find(a);
        if (callMapIt == callMap.end()) BUG("No callMap for %2%", a);
        return callMapIt->second;
    }

    Graphs::vertex_t rootVar;
    ProgramVarInfo progVarInfo;

    Graphs::ProcOf procOf;
    Graphs::CallMap callMap;
    Graphs::ProcCallers procCallerMap;
    hvec_map<cstring, std::vector<Graphs::vertex_t>> caller;
    hvec_map<const IR::Node *, Graphs::vertex_t> defBy;
    hvec_map<cstring, Graphs::vertex_t> srcOf;
    std::vector<TabVertex> actionParams;
    std::vector<Graphs::VarEdge> retArgEdges;
};

class SuperGraphs : public Graphs {
 public:
    SuperGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                std::vector<Graph *> *controlGraphsArray,
                VarMap *graphVars,
                hvec_map<cstring, VarMap> *graphLocalVars,
                hvec_map<cstring, Graphs::ProcOf> *procOfs,
                hvec_map<cstring, Graphs::CallMap> *callMaps,
                hvec_map<cstring, Graphs::ProcCallers> *procCallerMaps,
                hvec_map<cstring, std::vector<Graphs::VarEdge>> *retArgEdges)
    : refMap(refMap),
      typeMap(typeMap),
      controlGraphsArray(controlGraphsArray),
      graphVars(graphVars),
      graphLocalVars(graphLocalVars),
      procOfs(procOfs),
      callMaps(callMaps),
      procCallerMaps(procCallerMaps),
      retArgEdges(retArgEdges) {}

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
    VarMap *graphVars;
    hvec_map<cstring, VarMap> *graphLocalVars;
    hvec_map<cstring, Graphs::ProcOf> *procOfs;
    hvec_map<cstring, Graphs::CallMap> *callMaps;
    hvec_map<cstring, Graphs::ProcCallers> *procCallerMaps;
    hvec_map<cstring, std::vector<Graphs::VarEdge>> *retArgEdges;

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
