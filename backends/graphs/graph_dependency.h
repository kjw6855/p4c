/**
 * @author Jiwon Kim
 */

#include <map>
#include <queue>
#include <vector>

#include <boost/graph/breadth_first_search.hpp>
#include <boost/graph/visitors.hpp>

#include "graphs.h"

#ifndef BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_
#define BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_

namespace P4::graphs {

class GraphDependency : public Graphs {
 public:
    struct PathVariables {
        std::vector<const IR::Node *> input_var;
        std::vector<const IR::Node *> output_var;
    };

    GraphDependency(P4::ReferenceMap *refMap, P4::TypeMap *typeMap)
        : refMap(refMap), typeMap(typeMap) {}

    std::vector<Graphs::vertex_t> get_vertices_per_type(Graph *g, VertexType type, bool isStateful);

    std::vector<Graphs::vertex_t> find_path_from_vertices(Graph *g, Graphs::vertex_t &sv, Graphs::vertex_t &dv);

    void process(std::vector<Graph *> &controlGraphsArray);

 private:
    bool is_stateful(const IR::Node *node);

    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;
};
}  // namespace P4::graphs
#endif /* BACKENDS_GRAPHS_GRAPH_DEPENDENCY_H_ */
