#ifndef BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_
#define BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_

#include <memory>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>

#include "graphs.h"
#include "ide_pass.h"
#include "ir/ir.h"

namespace P4::P4StateDependency {

/// @class DependencyGraphs
/// @brief A specialized graph class that shows only data flow dependencies.
///
/// This class creates a simplified graph representation that focuses exclusively on
/// dependency relationships between variables. Unlike the full Graphs class which
/// includes control flow, procedure calls, and other edge types, DependencyGraphs
/// provides a clean view of what depends on what.
class DependencyGraphs {
 public:
    /// Vertex properties for the dependency graph
    struct DependencyVertex {
        cstring name;                    // Variable or node name
        std::optional<Graphs::vertex_t> graphId = std::nullopt;        // ID of the original graph vertex this corresponds to
        const IR::Node *node = nullptr;            // Associated IR node
        cstring color;                   // Color for visualization
        cstring shape;                   // Shape for visualization
    };

    /// Edge properties for the dependency graph
    struct DependencyEdge {
        cstring label;                   // Edge label (e.g., "depends_on")
        cstring style;                   // Edge style for visualization
    };

    /// Graph types used for representation
    using GraphvizAttributes = std::map<cstring, cstring>;
    using vertexProperties = boost::property<boost::vertex_attribute_t, GraphvizAttributes, DependencyVertex>;
    using edgeProperties =
        boost::property<boost::edge_name_t, cstring,
        boost::property<boost::edge_index_t, int,
        boost::property<boost::edge_attribute_t, GraphvizAttributes, DependencyEdge>>>;
    using graphProperties =
        boost::property<boost::graph_name_t, std::string,
        boost::property<boost::graph_graph_attribute_t, GraphvizAttributes,
        boost::property<boost::graph_vertex_attribute_t, GraphvizAttributes,
        boost::property<boost::graph_edge_attribute_t, GraphvizAttributes>>>>;
    using DepGraph_ = boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS,
                                         vertexProperties, edgeProperties, graphProperties>;
    using DepGraph = boost::subgraph<DepGraph_>;
    using edge_t = boost::graph_traits<DepGraph>::edge_descriptor;
    using vertex_t = boost::graph_traits<DepGraph>::vertex_descriptor;

    /// Constructor
    DependencyGraphs(size_t numGraphs);

    /// Destructor
    ~DependencyGraphs() = default;

    /// @brief Add a vertex representing a variable/node
    /// @param nodeId ID of the original graph vertex this corresponds to
    /// @param name Variable name
    /// @param node Associated IR node (optional)
    /// @return vertex descriptor
    vertex_t add_vertex(size_t index, std::optional<Graphs::vertex_t> nodeId,
        const cstring &name, const IR::Node *nodePtr = nullptr);

    /// @brief Add a dependency edge from source to target
    /// @param from Source variable vertex
    /// @param to Target variable vertex (depends on source)
    /// @param label Edge label
    /// @return edge descriptor
    edge_t add_dependency_edge(size_t index, vertex_t from, vertex_t to,
        const cstring &label = "depends_on"_cs);

    /// @brief Add dependencies from a DepEdgeMap
    /// @param graph The control flow graph for context
    /// @param depEdges The dependency edge map
    void add_dependencies_from_map(size_t index, Graphs::Graph *graph,
                                   const IDEPass::DepEdgeMap &depEdges, bool hasLeaves = false);

    /// @brief Get the underlying boost graph
    /// @return Reference to the dependency graph
    DepGraph &get_graph(size_t index) { return *depGraphs[index]; }
    const DepGraph &get_graph(size_t index) const { return *depGraphs[index]; }

    /// @brief Export the dependency graph to Graphviz DOT format
    /// @param filepath Path where the DOT file will be written
    void export_to_graphviz(size_t index, const std::filesystem::path &filepath) const;

    /// @brief Export the dependency graph to Graphviz DOT format (string)
    /// @return DOT format string
    std::string export_to_graphviz_string(size_t index) const;

    /// @brief Set vertex attributes for visualization
    /// @param v Vertex descriptor
    /// @param color Color name or hex code
    /// @param shape Shape name (box, circle, ellipse, etc.)
    void set_vertex_attributes(size_t index, vertex_t v, const cstring &color,
                              const cstring &shape = "box"_cs);

    /// @brief Get number of vertices in the graph
    size_t num_vertices(size_t index) const;

    /// @brief Get number of edges in the graph
    size_t num_edges(size_t index) const;

    /// @brief Remove nodes that cannot reach any leaf (out-degree 0) node.
    /// @param index Dependency graph index
    void prune_nodes_not_reaching_leaves(size_t index);

    class GraphAttributeSetter {
     public:
        void operator()(DepGraph &g) const {
            auto vertices = boost::vertices(g);
            for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
                const auto &vinfo = g[*vit];
                auto attrs = boost::get(boost::vertex_attribute, g);
                attrs[*vit]["label"_cs] = vinfo.name;
                attrs[*vit]["fillcolor"_cs] = vinfo.color;
                attrs[*vit]["shape"_cs] = vinfo.shape;
            }

            auto edges = boost::edges(g);
            for (auto &eit = edges.first; eit != edges.second; ++eit) {
                const auto &einfo = g[*eit];
                auto attrs = boost::get(boost::edge_attribute, g);
                attrs[*eit]["label"_cs] = einfo.label;
                attrs[*eit]["style"_cs] = einfo.style;
            }
        }
    };


 private:
    std::vector<std::unique_ptr<DepGraph>> depGraphs;

    /// Per-graph map from IR::Node pointer to vertex descriptor for fast lookup
    std::vector<std::unordered_map<const IR::Node *, vertex_t>> nodeToVertexMaps;

    /// Per-graph map from CFG vertex ID to dependency graph vertex descriptor
    std::vector<std::unordered_map<Graphs::vertex_t, vertex_t>> idToVertexMaps;

 public:
    std::vector<std::vector<vertex_t>> leaves;  // Per-graph list of leaf vertices (out-degree 0)
};

}  // namespace P4::P4StateDependency

#endif  /* BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_ */
