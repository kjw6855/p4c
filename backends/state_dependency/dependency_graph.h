#ifndef BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_
#define BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_

#include <memory>

#include <boost/dynamic_bitset.hpp>
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
    using EsgId = std::pair<Graphs::vertex_t, const IR::Node *>;

    enum class DepEdgeType {
        DEPENDS_ON,
        SATELITE,
        CHAIN_PATH,
        CALL_TO_RET,
    };

    /// Vertex properties for the dependency graph
    struct DependencyVertex {
        cstring name;                    // Variable or node name
        EsgId esgId;                    // Original graph vertex and IR node this corresponds to (for traceability)
        cstring color;                   // Color for visualization
        cstring shape;                   // Shape for visualization
        bool isSO = false;               // True for [SO] stateful-object vertices
        bool isSatellite = false;        // True for satellite circle nodes (category markers)
    };

    /// Edge properties for the dependency graph
    struct DependencyEdge {
        DepEdgeType type;                // Type of dependency edge
        cstring label;                   // Edge label (e.g., "depends_on")
        cstring style;                   // Edge style for visualization
        cstring color;                   // Edge color (empty = default black)
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
    vertex_t add_vertex(size_t index, EsgId esgId, const cstring &name, const cstring &color = ""_cs);

    /// @brief Add a vertex representing a stateful object
    /// @param soNode IR node of the stateful object
    /// @return vertex descriptor
    vertex_t add_so_vertex(size_t index, const IR::Node *soNode);

    /// @brief Add a dependency edge from source to target
    /// @param from Source variable vertex
    /// @param to Target variable vertex (depends on source)
    /// @param label Edge label
    /// @return edge descriptor
    edge_t add_dependency_edge(size_t index, vertex_t from, vertex_t to,
                               const cstring &label = ""_cs,
                               DepEdgeType type = DepEdgeType::DEPENDS_ON);

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

    /// @brief Add additional edges from constant writes to stateful objects
    void add_so_constant_edges(size_t index, Graphs::Graph *esg);

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

    /// @brief Remove CALL vertices that do not have their corresponding CALL_TO_RET edge
    /// @param index Dependency graph index
    void prune_call_nodes_without_return(size_t index, Graphs::Graph *esg);

    /// Per-register dependency chain: write-side and read-side vertices separated.
    /// One SOChain per SO vertex that has at least one incoming "write_to" edge.
    ///
    /// isUpdate: true when the SO's write source is the same action instance as its read
    /// destination (i.e. the action atomically reads-modifies-writes the register).
    /// Chains with isUpdate=true come in two flavours emitted separately:
    ///   1. readVertices == {soVertex} only  →  single-update test (1 execution).
    ///   2. readVertices has downstream pure readers  →  update-then-read (≥2 executions).
    /// writeVertices covers both the non-update write path and the update action itself;
    /// callers do not need to distinguish them from the write side.
    struct SOChain {
        vertex_t soVertex;
        cstring soName;
        const IR::Node *soNode;
        /// Dep-graph vertex info of the chain's sink (the KEY or header leaf).
        /// For non-update chains: taken from the leaf dep vertex (out_degree 0) in readVertices.
        /// For update-only chains: taken from the leaf dep vertex in the forward expansion of
        /// writeVertices (vertex reachable beyond the write_to edge, i.e. not in backward BFS
        /// context). esgId.first is the ESG vertex; check (*esg)[esgId.first].flags for KEY.
        /// Default-initialized (esgId.second == nullptr) when no sink vertex is found.
        DependencyVertex sinkNode;
        /// Control-plane name of the table whose key is the sink of this chain.
        /// Populated during construction by walking the ESG backward from the KEY sinkNode
        /// to its parent TABLE vertex. Empty for non-KEY chains (e.g. header sinks).
        cstring sinkTableControlPlaneName;
        /// Control-plane name of the specific key field in sinkTableControlPlaneName that
        /// corresponds to sinkNode (i.e. the key element whose expression is equiv to
        /// sinkNode.esgId.second). Empty when no matching key element is found.
        cstring sinkKeyName;
        /// For CONDITION sinks (H2S2C): the IR::IfStatement whose condition the SO value feeds.
        /// Resolved from (*esg)[sinkNode.esgId.first].node during construction. Null for KEY/SWITCH.
        const IR::Node *sinkConditionNode = nullptr;
        /// Vertices on the write side: the EXIT node that writes to this SO plus its full
        /// backward-reachable context (including the update action body when isUpdate=true).
        std::unordered_set<vertex_t> writeVertices;
        /// {soVertex} plus all vertices forward-reachable via pure "read_from" edges.
        /// For single-update chains this is {soVertex} only.
        std::unordered_set<vertex_t> readVertices;
        /// True when the same action instance both reads and writes this SO.
        bool isUpdate = false;

        /// Chain index assigned by analysis.cpp after graph resolution.
        size_t id = 0;

        /// IR-node sets resolved from writeVertices/readVertices by analysis.cpp.
        /// Empty until resolveSOChainNodes() fills them in.
        std::map<vertex_t, const IR::Node *> writeNodes;
        boost::dynamic_bitset<> writeNodeIds;
        std::map<vertex_t, const IR::Node *> readNodes;
        boost::dynamic_bitset<> readNodeIds;

        /// --parser-deps: per-chain header pins. When the chain is rooted at a parser-derived metadata
        /// field, each entry records a header field whose value (through the parser states) determines that
        /// metadata (writePath=true -> write/phase-2 path; writePath=false -> read/phase-1 path). Stored as
        /// field-path strings (e.g. "hdr.ipv4.id"), stable across tools/unroll. Empty for single-control /
        /// whole-pipeline chains.
        /// NOTE: the per-phase header pinning itself is AUTOMATIC in p4symbex — each phase's emitted input
        /// packet is the concrete packet from that phase's model (state_dependency_track.cpp), and since
        /// p4symbex executes the parser, the model constrains the headers so the parser produces the chain's
        /// metadata. These pins are therefore for labeling / future explicit cross-phase coordination.
        struct ParserDep {
            cstring metaPath;
            cstring hdrPath;
            bool writePath = false;
        };
        std::vector<ParserDep> parserDeps;

        /// Default constructor for reconstructing a chain from a serialized cache
        /// (chain_cache.cpp): only the symbex-consumed fields (soName, sinkTableControlPlaneName,
        /// sinkKeyName, isUpdate, id, writeNodes, readNodes, sinkConditionNode) are set; the graph
        /// vertices and clone_id bitsets are left default (symbex never reads them).
        SOChain() = default;

        SOChain(DepGraph *depG, Graphs::Graph *esg,
                vertex_t soVertex, cstring soName, const IR::Node *soNode,
                std::unordered_set<vertex_t> writeVertices,
                std::unordered_set<vertex_t> readVertices,
                bool isUpdate, size_t id,
                DependencyVertex sinkNode)
            : soVertex(soVertex), soName(soName), soNode(soNode), sinkNode(sinkNode),
              writeVertices(std::move(writeVertices)),
              readVertices(std::move(readVertices)), isUpdate(isUpdate), id(id) {
            for (auto v : this->writeVertices) {
                auto esgVtx = (*depG)[v].esgId.first;
                if (esgVtx >= boost::num_vertices(*esg)) continue;
                const auto *irNode = (*esg)[esgVtx].node;
                if (irNode == nullptr) continue;
                writeNodes.emplace(v, irNode);
                auto cloneId = static_cast<size_t>(irNode->clone_id);
                if (cloneId >= writeNodeIds.size()) writeNodeIds.resize(cloneId + 1, false);
                writeNodeIds.set(cloneId);
            }
            for (auto v : this->readVertices) {
                auto esgVtx = (*depG)[v].esgId.first;
                if (esgVtx >= boost::num_vertices(*esg)) continue;
                const auto *irNode = (*esg)[esgVtx].node;
                if (irNode == nullptr) continue;
                readNodes.emplace(v, irNode);
                auto cloneId = static_cast<size_t>(irNode->clone_id);
                if (cloneId >= readNodeIds.size()) readNodeIds.resize(cloneId + 1, false);
                readNodeIds.set(cloneId);
            }
            // Walk ESG backward from the KEY sink vertex to find the parent TABLE vertex.
            // Record its control-plane name and the specific key field name that matches
            // sinkNode.esgId.second for use in Phase 3 forbidden-value filtering.
            auto sinkEsgVtx = sinkNode.esgId.first;
            if (sinkEsgVtx < boost::num_vertices(*esg) &&
                hasFlag((*esg)[sinkEsgVtx].flags, VertexFlags::KEY)) {
                for (auto [ei, ee] = boost::in_edges(sinkEsgVtx, *esg); ei != ee; ++ei) {
                    auto src = boost::source(*ei, *esg);
                    if (hasFlag((*esg)[src].flags, VertexFlags::TABLE)) {
                        const auto *tbl = (*esg)[src].node->to<IR::P4Table>();
                        if (tbl == nullptr) break;
                        sinkTableControlPlaneName = tbl->controlPlaneName();
                        const auto *sinkVar = sinkNode.esgId.second;
                        const IR::Key *key = tbl->getKey();
                        if (sinkVar != nullptr && key != nullptr) {
                            for (const auto *keyElem : key->keyElements) {
                                if (!keyElem->expression->equiv(*sinkVar)) continue;
                                const auto *nameAnnot = keyElem->getAnnotation(
                                    IR::Annotation::nameAnnotation);
                                if (nameAnnot != nullptr)
                                    sinkKeyName = nameAnnot->getName();
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            // For CONDITION sinks (H2S2C), record the IfStatement so symbex can evaluate the
            // condition value and compare the then/else branch effects.
            if (sinkEsgVtx < boost::num_vertices(*esg) &&
                hasFlag((*esg)[sinkEsgVtx].flags, VertexFlags::CONDITION)) {
                sinkConditionNode = (*esg)[sinkEsgVtx].node;
            }
        };
    };

    /// Return all vertices forward-reachable from SO vertices that have NO incoming
    /// "write_to" edge (category 1: reads of non-written registers).
    std::vector<SOChain> get_nowrite_so_vertices(
        size_t index, Graphs::Graph *esg) const;

    /// Return all vertices in data-write chains (paths that contain at least one
    /// non-SO → SO "write_to" edge, category 2/3).
    /// If @p soNames is non-null it is populated with the name of every written SO.
    std::vector<SOChain> get_data_write_so_chains(
        size_t index, Graphs::Graph *esg,
        std::unordered_set<cstring> *soNames = nullptr) const;

    /// @brief Add a small satellite circle node beside every vertex in each chain category.
    /// For each (vertex, chain) pair a circle node is added with the chain ID as its label
    /// and a color matching the vertex's role.  An edge from vertex → satellite carries
    /// the same chain ID label.
    /// @return (original_vertex, satellite_vertex) pairs for rank=same injection.
    std::vector<std::pair<vertex_t, vertex_t>> add_chain_satellites(size_t index,
        const std::vector<SOChain> &readChains, const std::vector<SOChain> &writeChains);

    /// @brief Inject {rank=same; orig; sat;} directives into an already-written DOT file.
    static void inject_rank_groups(const std::filesystem::path &filepath,
                                   const std::vector<std::pair<vertex_t, vertex_t>> &pairs);

    /// @brief Merge nodes without variables into nodes with variables
    /// @param index Dependency graph index
    void merge_nodes_without_variable(size_t index);

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
                attrs[*vit]["style"_cs] = "filled"_cs;
                if (vinfo.isSatellite) {
                    attrs[*vit]["fixedsize"_cs] = "true"_cs;
                    attrs[*vit]["width"_cs] = "0.3"_cs;
                    attrs[*vit]["height"_cs] = "0.3"_cs;
                    attrs[*vit]["fontsize"_cs] = "10"_cs;
                }
            }

            auto edges = boost::edges(g);
            for (auto &eit = edges.first; eit != edges.second; ++eit) {
                const auto &einfo = g[*eit];
                auto attrs = boost::get(boost::edge_attribute, g);
                attrs[*eit]["label"_cs] = einfo.label;
                attrs[*eit]["style"_cs] = einfo.style;
                if (!einfo.color.isNullOrEmpty()) {
                    attrs[*eit]["color"_cs] = einfo.color;
                    attrs[*eit]["fontcolor"_cs] = einfo.color;
                }
            }
        }
    };


 private:
    using EsgToDepMap = std::unordered_map<EsgId, vertex_t>;
    std::vector<std::unique_ptr<DepGraph>> depGraphs;

    // Per-graph map from (original graph vertex, IR node) to dependency graph vertex
    std::vector<EsgToDepMap> esgToDepMaps;

    void prune_and_remap(size_t index, std::unordered_set<vertex_t> &keep);

 public:
    std::vector<std::vector<vertex_t>> leaves;  // Per-graph list of leaf vertices (out-degree 0)
};

}  // namespace P4::P4StateDependency

#endif  /* BACKENDS_STATE_DEPENDENCY_DEPENDENCY_GRAPH_H_ */
