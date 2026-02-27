#ifndef BACKENDS_STATE_DEPENDENCY_GRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_GRAPHS_H_

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graphviz.hpp>

#include "frontends/p4/frontend.h"
#include "frontends/p4/parserCallGraph.h"
#include "ir/ir.h"
#include "ir/visitor.h"
#include "lib/hvec_map.h"
#include "lib/hvec_set.h"

namespace P4 {

class ReferenceMap;
class TypeMap;

}  // namespace P4

namespace P4::P4StateDependency {

using namespace P4::literals;

enum class VertexFlags : unsigned {
    NONE            = 0,
    TABLE           = 1u << 0,
    KEY             = 1u << 1,
    ACTION          = 1u << 2,
    CONDITION       = 1u << 3,
    SWITCH          = 1u << 4,
    STATEMENT       = 1u << 5,
    CONTROL         = 1u << 6,
    PARSER_STATE    = 1u << 7,
    STATEFUL        = 1u << 8,
    CALL            = 1u << 9,
    RETURN          = 1u << 10,
    ENTRY           = 1u << 11,
    EXIT            = 1u << 12,
    EMPTY           = 1u << 13,
    VARIABLE        = 1u << 14,
};

enum class EdgeType {
    CONTROL,
    CALL_TO_RETURN,
    INTER_PROCEDURE,
    DEFUSE,
    IFDS_FT,
    IFDS,
    HAS_VAR,
};

inline cstring edgeTypeToString(EdgeType type) {
    switch (type) {
        case EdgeType::CONTROL:
            return "CONTROL"_cs;
        case EdgeType::CALL_TO_RETURN:
            return "CALL_TO_RETURN"_cs;
        case EdgeType::INTER_PROCEDURE:
            return "INTER_PROCEDURE"_cs;
        case EdgeType::DEFUSE:
            return "DEFUSE"_cs;
        case EdgeType::IFDS:
        case EdgeType::IFDS_FT:
            return "IFDS"_cs;
        case EdgeType::HAS_VAR:
            return "HAS_VAR"_cs;
        default:
            break;
    }
    return cstring::empty;
}

class EdgeTypeIface {
 public:
    cstring name;
    EdgeType type;

    EdgeTypeIface() {}
    EdgeTypeIface(EdgeType type) : type(type) {}
    EdgeTypeIface(cstring name, EdgeType type)
        : name(name), type(type) {}

    virtual ~EdgeTypeIface() {}
};

class EdgeProcedural : public EdgeTypeIface {
 public:
    EdgeProcedural()
        : EdgeTypeIface(cstring::empty, EdgeType::INTER_PROCEDURE) {}
};

class EdgeUnconditional : public EdgeTypeIface {
 public:
    EdgeUnconditional()
        : EdgeTypeIface(cstring::empty, EdgeType::CONTROL) {}
};

class EdgeIf : public EdgeTypeIface {
 public:
    EdgeIf(bool isTrue)
        : EdgeTypeIface(isTrue ? "TRUE"_cs : "FALSE"_cs,
                EdgeType::CONTROL) {}
};

class EdgeSwitch : public EdgeTypeIface {
 public:
    EdgeSwitch(const IR::Expression *labelExpr)
        : EdgeTypeIface(EdgeType::CONTROL),
          labelExpr(labelExpr) {
        std::stringstream sstream;
        labelExpr->dbprint(sstream);
        name = cstring(sstream);
    }
 private:
    const IR::Expression *labelExpr;
};
class EdgeVar : public EdgeTypeIface {
 public:
    EdgeVar()
        : EdgeTypeIface(cstring::empty, EdgeType::HAS_VAR) {}
};

inline VertexFlags operator|(VertexFlags a, VertexFlags b) {
    return static_cast<VertexFlags>(static_cast<unsigned>(a) |
            static_cast<unsigned>(b));
}
// OR-assign
inline VertexFlags& operator|=(VertexFlags& a, VertexFlags b) {
    a = a | b;
    return a;
}
inline bool hasFlag(VertexFlags v, VertexFlags f) {
    return (static_cast<unsigned>(v) & static_cast<unsigned>(f)) != 0;
}
inline cstring vertexFlagsToString(VertexFlags flags) {
    if (flags == VertexFlags::NONE) return "NONE"_cs;

    std::vector<cstring> parts;

    if (hasFlag(flags, VertexFlags::TABLE))        parts.emplace_back("TABLE"_cs);
    if (hasFlag(flags, VertexFlags::KEY))          parts.emplace_back("KEY"_cs);
    if (hasFlag(flags, VertexFlags::ACTION))       parts.emplace_back("ACTION"_cs);
    if (hasFlag(flags, VertexFlags::CONDITION))    parts.emplace_back("CONDITION"_cs);
    if (hasFlag(flags, VertexFlags::SWITCH))       parts.emplace_back("SWITCH"_cs);
    if (hasFlag(flags, VertexFlags::STATEMENT))    parts.emplace_back("STATEMENT"_cs);
    if (hasFlag(flags, VertexFlags::CONTROL))      parts.emplace_back("CONTROL"_cs);
    if (hasFlag(flags, VertexFlags::PARSER_STATE)) parts.emplace_back("PARSER_STATE"_cs);
    if (hasFlag(flags, VertexFlags::STATEFUL))     parts.emplace_back("STATEFUL"_cs);
    if (hasFlag(flags, VertexFlags::CALL))         parts.emplace_back("CALL"_cs);
    if (hasFlag(flags, VertexFlags::RETURN))       parts.emplace_back("RETURN"_cs);
    if (hasFlag(flags, VertexFlags::ENTRY))        parts.emplace_back("ENTRY"_cs);
    if (hasFlag(flags, VertexFlags::EXIT))         parts.emplace_back("EXIT"_cs);
    if (hasFlag(flags, VertexFlags::VARIABLE))     parts.emplace_back("VARIABLE"_cs);

    cstring res;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) res += "|"_cs;
        res += parts[i];
    }
    return res;
}

class Graphs {
 public:
    struct Vertex {
        cstring name;
        VertexFlags flags;
        const IR::Node *node;
        std::vector<const IR::Node *> defVars;
        std::vector<const IR::Node *> useVars;
    };

    hvec_map<cstring, hvec_set<const IR::Node *>> graphVars;

    using GraphvizAttributes = std::map<cstring, cstring>;
    using vertexProperties = boost::property<boost::vertex_attribute_t, GraphvizAttributes, Vertex>;
    using edgeProperties =
        boost::property<boost::edge_name_t, cstring,
        boost::property<boost::edge_index_t, int,
        boost::property<boost::edge_attribute_t, GraphvizAttributes, EdgeTypeIface>>>;
    using graphProperties =
        boost::property<boost::graph_name_t, std::string,
        boost::property<boost::graph_graph_attribute_t, GraphvizAttributes,
        boost::property<boost::graph_vertex_attribute_t, GraphvizAttributes,
        boost::property<boost::graph_edge_attribute_t, GraphvizAttributes>>>>;
    using Graph_ = boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS,
                                         vertexProperties, edgeProperties, graphProperties>;
    using Graph = boost::subgraph<Graph_>;
    using edge_t = boost::graph_traits<Graph>::edge_descriptor;
    using vertex_t = boost::graph_traits<Graph>::vertex_descriptor;

    using Parents = std::vector<std::pair<vertex_t, EdgeTypeIface *>>;

    using IndexMap = boost::property_map<Graph, boost::vertex_index_t>::type;

    vertex_t add_vertex(const cstring &name, VertexFlags flags, const IR::Node *node=nullptr);

    void add_edge(const vertex_t &from, const vertex_t &to, const cstring &name, EdgeType type);

    void add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                  EdgeType type, unsigned cluster_id);

    vertex_t add_and_connect_vertex(const cstring &name, VertexFlags flags,
                                    const IR::Node *node=nullptr);

    void add_and_connect_vertex(Graphs::vertex_t &target, EdgeType edgeType);

    cstring get_var_name(const IR::Node *var) {
        std::stringstream sstream;
        sstream << var;
        auto fullName = cstring(sstream);
        if (auto *p = fullName.findlast(' ')) return cstring(p + 1);
        return fullName;
    }

    vertex_t add_var_vertex(const IR::Node *var, std::optional<const vertex_t> node=std::nullopt) {
        cstring vname = get_var_name(var);
        auto vv = add_vertex(vname, VertexFlags::VARIABLE, var);

        if (node.has_value())
            add_edge(node.value(), vv, cstring::empty, EdgeType::HAS_VAR);

        return vv;
    }

    std::optional<vertex_t> add_variable_in_vertex(const IR::Node *var, const vertex_t &v,
            bool isUsed) {
        /* check duplicate variable in set */
        auto &varset = graphVars[graphName];
        auto *newVar = var;
        for (auto *inVar : varset) {
            if (inVar->equiv(*var)) {
                newVar = inVar;
                break;
            }
        }
        // Insert new variable
        if (newVar == var)
            graphVars[graphName].insert(var);

        if (showVar || genSupergraphs) {
            auto &vinfo = (*g)[v];
            auto &varList = isUsed ? vinfo.useVars : vinfo.defVars;
            varList.push_back(newVar);

            if (showVar && !genSupergraphs)
                return add_var_vertex(newVar, v);
        }
        return {};
    }

    vertex_t get_root_vertex(Graph *g) {
        return *boost::vertices(*g).first;
    }

    class GraphAttributeSetter {
     public:
        void operator()(Graph &g, bool showVar=false) const {
            auto vertices = boost::vertices(g);
            for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
                const auto &vinfo = g[*vit];
                auto attrs = boost::get(boost::vertex_attribute, g);
                cstring labelName = vinfo.name;
                if (vinfo.flags == VertexFlags::VARIABLE) {
                    attrs[*vit]["label"_cs] = cstring::empty;
                    attrs[*vit]["xlabel"_cs] = labelName;
                    attrs[*vit]["fixedsize"_cs] = "true"_cs;
                } else {
                    attrs[*vit]["label"_cs] = labelName;
                }
                attrs[*vit]["style"_cs] = vertexFlagGetStyle(vinfo.flags, showVar);
                attrs[*vit]["fillcolor"_cs] = vertexFlagGetColor(g, *vit);
                attrs[*vit]["shape"_cs] = vertexFlagGetShape(vinfo.flags);
                attrs[*vit]["width"_cs] = vertexFlagGetWidth(vinfo.flags);
                attrs[*vit]["margin"_cs] = vertexFlagGetMargin();
            }

            auto edges = boost::edges(g);
            for (auto &eit = edges.first; eit != edges.second; ++eit) {
                auto attrs = boost::get(boost::edge_attribute, g);
                auto ep = g[*eit];
                attrs[*eit]["label"_cs] = ep.name;
                attrs[*eit]["style"_cs] = edgeTypeGetStyle(ep.type);
                attrs[*eit]["color"_cs] = edgeTypeGetColor(ep.type);
                attrs[*eit]["penwidth"_cs] = edgeTypeGetPenWidth(ep.type);
            }
        }

     private:
        static cstring vertexFlagGetShape(VertexFlags flags) {
            if (hasFlag(flags, VertexFlags::VARIABLE))
                return "circle"_cs;
            if (hasFlag(flags, VertexFlags::TABLE) ||
                    hasFlag(flags, VertexFlags::ACTION))
                return "ellipse"_cs;

            return "rectangle"_cs;
        }
        static cstring vertexFlagGetStyle(VertexFlags flags, bool showVar) {
            if (hasFlag(flags, VertexFlags::CONTROL))
                return "dashed"_cs;
            else if (hasFlag(flags, VertexFlags::EMPTY))
                return "invis"_cs;
            else if (hasFlag(flags, VertexFlags::CONDITION))
                return "rounded"_cs;
            else if (hasFlag(flags, VertexFlags::KEY))
                return "rounded"_cs;
            else if (hasFlag(flags, VertexFlags::SWITCH))
                return "rounded"_cs;
            else if (hasFlag(flags, VertexFlags::TABLE))
                return "filled"_cs;
            else if (hasFlag(flags, VertexFlags::STATEFUL))
                return "filled"_cs;
            if (hasFlag(flags, VertexFlags::VARIABLE)) {
                if (showVar)
                    return "filled"_cs;
                else
                    return "invis"_cs;
            }

            return "solid"_cs;
        }
        cstring vertexFlagGetColor(Graph &g, const vertex_t &v) const {
            const auto &vinfo = g[v];
            auto flags = vinfo.flags;
            cstring colorName = cstring::empty;
            if (hasFlag(flags, VertexFlags::TABLE))
                colorName = "lightsalmon"_cs;
            if (hasFlag(flags, VertexFlags::STATEFUL))
                colorName = "lightgreen"_cs;
            if (hasFlag(flags, VertexFlags::VARIABLE)) {
                if (boost::out_degree(v, g) > 0) return "black"_cs;
                bool filled = false;
                for (auto [ei, ei_end] = boost::in_edges(v, g); ei != ei_end; ++ei) {
                    auto edge = g[*ei];
                    if (edge.type != EdgeType::HAS_VAR) {
                        filled = true;
                        break;
                    }
                }

                colorName = filled ? "black"_cs : "white"_cs;
            }
            return colorName;
        }
        static cstring vertexFlagGetWidth(VertexFlags flags) {
            if (hasFlag(flags, VertexFlags::VARIABLE))
                return "0.2"_cs;
            return cstring::empty;
        }
        static cstring vertexFlagGetMargin() {
            return cstring::empty;
        }
        static cstring edgeTypeGetStyle(EdgeType type) {
            switch (type) {
                case EdgeType::INTER_PROCEDURE:
                    return "dotted"_cs;
                case EdgeType::CALL_TO_RETURN:
                    return "bold"_cs;
                case EdgeType::DEFUSE:
                    return "dashed"_cs;
                case EdgeType::HAS_VAR:
                    return "invis"_cs;
                case EdgeType::IFDS:
                    return "bold"_cs;
                default:
                    break;
            }
            return cstring::empty;
        }
        static cstring edgeTypeGetPenWidth(EdgeType type) {
            switch (type) {
                case EdgeType::INTER_PROCEDURE:
                    return "2"_cs;
                default:
                    break;
            }
            return cstring::empty;
        }
        static cstring edgeTypeGetColor(EdgeType type) {
            switch (type) {
                case EdgeType::DEFUSE:
                case EdgeType::IFDS_FT:
                    return "grey"_cs;
                default:
                    break;
            }
            return cstring::empty;
        }
    };  // end class GraphAttributeSetter

 protected:
    Graph *g{nullptr};
    vertex_t start_v{};
    vertex_t exit_v{};
    Parents parents{};
    cstring graphName;
 public:
    bool showVar;
    bool genSupergraphs;
    static const IR::Node *globalNode;
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_GRAPHS_H_ */
