/**
 * @author Jiwon Kim
 */

#include <map>
#include <queue>
#include <vector>

#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/graph/visitors.hpp>

#include "graphs.h"
#include "def_use.h"

#ifndef BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_
#define BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_

namespace P4::graphs {

class GraphDependency : public Graphs {
 public:
    struct PathVariables {
        std::vector<const IR::Node *> input_var;
        std::vector<const IR::Node *> output_var;
    };

    GraphDependency(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                    ComputeDefUse *defUse,
                    std::vector<Graph *> &controlGraphsArray,
                    bool splitVertex)
        : refMap(refMap),
          typeMap(typeMap),
          defUse(defUse),
          controlGraphsArray(controlGraphsArray),
          splitVertex(splitVertex) {}

    std::vector<Graphs::vertex_t> get_vertices_per_type(Graph *g, VertexType type, bool isStateful);

    std::vector<Graphs::vertex_t> find_path_from_vertices(Graph *g, Graphs::vertex_t &sv, Graphs::vertex_t &dv);

    void process();
    void analyze();
    void dump_def_use();

 private:
    /** PDG Generators **/
    // Generate PDG for each subgraph
    void process_subgraph(Graph *g);

    // Find CFG vertex from defuse var loc
    std::vector<std::pair<Graphs::vertex_t, const IR::Node *>> find_node_by_loc(Graph *g, const ComputeDefUse::loc_t *loc);

    // Split CFG vertex based on defuse variables
    void split_cfg_vertices(Graph *g);
    void split_cfg_vertex(Graph *g, const Graphs::vertex_t &v,
                          hvec_map<const IR::Node *, ComputeDefUse::locset_t> &nodeToVarMap);

    // Add uses/defs in every vertex
    std::vector<Graphs::vertex_t> add_var_in_cfg(Graph *g, const ComputeDefUse::loc_t *loc, bool isDef);


    /** Security Analysis **/
    // Analyze dependencies for each subgraph
    void analyze_subgraph(Graph *g);

    // Collect all CFG paths with DFS
    void dfs_all_paths(Graph *g, Graphs::vertex_t &cur, Graphs::vertex_t &dst,
                       std::vector<Graphs::vertex_t> &path,
                       std::vector<std::vector<Graphs::vertex_t>> &all_paths);

    // Get path from MissAction to HitAction
    bool add_on_miss_defuse_path(Graph *g, Graphs::vertex_t &vit,
                                 std::vector<Graphs::vertex_t> &postPaths);

    // Find HitAction used by MissAction
    std::optional<Graphs::vertex_t> get_defuse_action(Graph *g, Graphs::vertex_t &vit,
                                                      std::vector<Graphs::vertex_t> &pathToDst);

    // Find all paths from sv to dv
    // Append Miss-to-Hit path if dv is add-on-miss table
    int find_all_paths(Graph *g, Graphs::vertex_t &sv, Graphs::vertex_t &dv,
                       std::vector<std::vector<Graphs::vertex_t>> &allPaths);

    bool dfs_table_so_policy(Graph *g,
                             Graphs::vertex_t src,
                             std::vector<Graphs::vertex_t> &statefulVertices,
                             varset_t &vars);

    bool check_table_so_policy(Graph *g, Graphs::vertex_t src,
                               std::vector<Graphs::vertex_t> &statefulVertices);

    void find_action_vertices(Graph *g, Graphs::vertex_t u,
                          std::vector<Graphs::vertex_t> &foundVertices,
                          bool storeNext);

    bool is_cyclic(Graph *g);
    std::size_t dfs_find_cycle(Graph *g, Graphs::vertex_t u,
        Graphs::IndexMap &index,
        std::vector<bool> &visited, std::vector<bool> &recStack,
        std::vector<Graphs::vertex_t> &found_vertices,
        std::vector<cstring> &found_edges);

    /** Misc **/
    // Check if the node is stateful
    bool is_stateful(const IR::Node *node);

    // Dump defuse variables
    void dump_vars_in_graph(Graph *g);

    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
    ComputeDefUse *defUse;
    std::vector<Graph *> &controlGraphsArray;
    bool splitVertex;
};
}  // namespace P4::graphs
#endif /* BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_ */
