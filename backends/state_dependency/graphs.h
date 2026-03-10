#ifndef BACKENDS_STATE_DEPENDENCY_GRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_GRAPHS_H_

#include <memory>

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

// TODO: minimize bit vector size
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
    STATEFUL        = 1u << 8,      // require SOFlags
    CALL            = 1u << 9,
    RETURN          = 1u << 10,
    ENTRY           = 1u << 11,
    EXIT            = 1u << 12,
    EMPTY           = 1u << 13,
    VARIABLE        = 1u << 14,
    SO_IDX          = 1u << 15,
    SO_DATA         = 1u << 16,
};

enum class SOFlags : unsigned {
    NONE            = 0,
    CREATE          = 1u << 0,
    READ            = 1u << 1,
    UPDATE          = 1u << 2,
    DELETE          = 1u << 3,      // TODO: unsupported
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

inline size_t get_top_value(size_t actIdNum) {
    return (actIdNum == 0) ? 0 : (size_t(1) << actIdNum) - 1;
}

// TODO: extend size_t to template<D>
class EdgeFunc {
 public:
    virtual size_t operator()(size_t i) const = 0;

    virtual std::unique_ptr<EdgeFunc> compose(std::unique_ptr<EdgeFunc> g) const {
        class ComposeFunc : public EdgeFunc {
         public:

            ComposeFunc(std::unique_ptr<EdgeFunc> o, std::unique_ptr<EdgeFunc> i)
                : outer(std::move(o)), inner(std::move(i)) {}

            size_t operator()(size_t x) const override {
                return (*outer)((*inner)(x));
            }

            cstring getName() const override {
                return outer->getName() + " U "_cs + inner->getName();
            }

            std::optional<size_t> getValue() const override {
                if (!inner && !outer) return {};
                else if (!inner) return outer->getValue();
                else if (!outer) return inner->getValue();

                auto outVal = outer->getValue();
                auto inVal = inner->getValue();
                if (!outVal.has_value()) return inVal;
                if (!inVal.has_value()) return outVal;

                // x | y
                size_t val = inVal.value() | outVal.value();
                return std::optional{val};
            }

            std::unique_ptr<EdgeFunc> clone() const override {
                // deep copy: clone outer and inner
                return std::make_unique<ComposeFunc>(
                        outer ? outer->clone() : nullptr,
                        inner ? inner->clone() : nullptr
                        );
            }

         private:
            std::unique_ptr<EdgeFunc> outer;
            std::unique_ptr<EdgeFunc> inner;
        };

        return std::make_unique<ComposeFunc>(
            std::unique_ptr<EdgeFunc>(this->clone()),
            std::move(g)
        );
    }

    virtual std::unique_ptr<EdgeFunc> clone() const = 0;
    virtual cstring getName() const = 0;
    virtual std::optional<size_t> getValue() const = 0;
    virtual ~EdgeFunc() = default;
};

class IdFunc : public EdgeFunc {
 public:
    size_t operator()(size_t i) const override { return i; }
    std::unique_ptr<EdgeFunc> clone() const override {
        return std::make_unique<IdFunc>(*this);
    }
    std::optional<size_t> getValue() const override { return std::nullopt; }
    cstring getName() const override { return "id"_cs; }
};

class ActionBitSetFunc : public EdgeFunc {
 public:
    explicit ActionBitSetFunc(size_t actBits) : actBits(actBits) {}

    // TODO: Change return type from size_t to bitVector
    size_t operator()(size_t i) const override {
        return i | actBits;
    }
    std::unique_ptr<EdgeFunc> clone() const override {
        return std::make_unique<ActionBitSetFunc>(*this);
    }
    cstring getName() const override {
        std::stringstream sstream;
        sstream << "set " << __builtin_popcountll(actBits) << " 1s";
        return cstring(sstream);
    }
    std::optional<size_t> getValue() const override {
        return std::optional{actBits};
    }

 private:
    size_t actBits;
};

// TODO: Move custom EdgeFunc to child pass (e.g., non_exact_to_stateful)
//       instead of common library like supergraph / tabulation
struct ActionSetFunc : public ActionBitSetFunc {
 public:
    explicit ActionSetFunc(size_t actId)
        : ActionBitSetFunc(size_t(1) << actId),
          actId(actId) {}
    std::unique_ptr<EdgeFunc> clone() const override {
        return std::make_unique<ActionSetFunc>(*this);
    }
    cstring getName() const override {
        std::stringstream sstream;
        sstream << "(i | (1 << " << actId << "))";
        return cstring(sstream);
    }

 private:
    size_t actId;
};

struct TopFunc : public EdgeFunc {
 public:
    explicit TopFunc(size_t actNum) : actNum(actNum) {
        BUG_CHECK(actNum < 64, "Out of range!");
        actBits = get_top_value(actNum);
    }
    size_t operator()(size_t) const override {
        // Top will absorb any bit index
        return actBits;
    }
    std::unique_ptr<EdgeFunc> clone() const override {
        return std::make_unique<TopFunc>(*this);
    }
    std::optional<size_t> getValue() const override { return std::optional{actBits}; }
    cstring getName() const override { return "T"_cs; }

 private:
    size_t actNum;
    size_t actBits;
};

struct BottomFunc : public EdgeFunc {
 public:
    explicit BottomFunc() : actBits(0) {}
    size_t operator()(size_t) const override {
        // Bottom will absorb any bit index
        return actBits;
    }
    std::unique_ptr<EdgeFunc> clone() const override {
        return std::make_unique<BottomFunc>(*this);
    }
    std::optional<size_t> getValue() const override { return std::optional{0}; }
    cstring getName() const override { return "0"_cs; }

 private:
    size_t actBits;
};

struct EdgeFuncHolder {
 public:
    EdgeFuncHolder() = default;

    EdgeFuncHolder(std::unique_ptr<EdgeFunc> f) : fn(std::move(f)) {}

    EdgeFuncHolder(const EdgeFuncHolder& other)
        : fn(other.fn ? other.fn->clone() : nullptr) {}

    EdgeFuncHolder& operator=(const EdgeFuncHolder& other) {
        if (this != &other) {
            fn = other.fn ? other.fn->clone() : nullptr;
        }
        return *this;
    }

    EdgeFuncHolder(EdgeFuncHolder&&) noexcept = default;
    EdgeFuncHolder& operator=(EdgeFuncHolder&&) noexcept = default;

    size_t operator()(size_t i) {
        return (*fn)(i);
    }

    cstring getName() {
        return fn->getName();
    }

    std::optional<size_t> getValue() {
        return fn->getValue();
    }

    EdgeFuncHolder compose(const EdgeFuncHolder &other) const {
        if (!fn) return EdgeFuncHolder(other.fn ? other.fn->clone() : nullptr);
        if (!other.fn) return EdgeFuncHolder(fn->clone());

        auto other_clone = other.fn->clone();
        auto composed = fn->compose(std::move(other_clone));
        return EdgeFuncHolder(std::move(composed));
    }

    EdgeFuncHolder may_join(const EdgeFuncHolder &other) const {
        if (!fn || !other.fn) {
            return EdgeFuncHolder(std::make_unique<BottomFunc>());
        }
        auto fVal = fn->getValue();
        auto gVal = other.fn->getValue();

        if (fVal.has_value() && gVal.has_value()) {
            if (fVal.value() == gVal.value()) {
                return EdgeFuncHolder(fn->clone());
            } else if (typeid(*fn) == typeid(TopFunc)){
                // T | x = x
                return EdgeFuncHolder(other.fn->clone());
            } else if (typeid(*other.fn) == typeid(TopFunc)) {
                // x | T = x
                return EdgeFuncHolder(fn->clone());
            } else {
                // x | y
                auto newVal = fVal.value() | gVal.value();
                if (newVal == 0) {
                    return EdgeFuncHolder(std::make_unique<BottomFunc>());
                } else {
                    return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(newVal));
                }
            }
        } else if (fVal.has_value()) {
            // 0 | Id = 0
            if (fVal.value() == 0)
                return EdgeFuncHolder(std::make_unique<BottomFunc>());
            // T | Id = Id
            else if (typeid(*fn) == typeid(TopFunc))
                return EdgeFuncHolder(std::make_unique<IdFunc>());

            // Id | x = x
            else
                return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(fVal.value()));
        } else if (gVal.has_value()) {
            // Id | 0 = 0
            if (gVal.value() == 0)
                return EdgeFuncHolder(std::make_unique<BottomFunc>());
            // Id | T = Id
            else if (typeid(*other.fn) == typeid(TopFunc))
                return EdgeFuncHolder(std::make_unique<IdFunc>());

            // Id | x = x
            else
                return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(gVal.value()));
        }
        // Id | Id = Id
        return EdgeFuncHolder(fn->clone());
    }

    EdgeFuncHolder must_join(const EdgeFuncHolder &other) const {
        if (!fn || !other.fn) {
            return EdgeFuncHolder(std::make_unique<BottomFunc>());
        }
        auto fVal = fn->getValue();
        auto gVal = other.fn->getValue();

        if (fVal.has_value() && gVal.has_value()) {
            if (fVal.value() == gVal.value()) {
                return EdgeFuncHolder(fn->clone());
            } else {
                // x v y
                auto newVal = fVal.value() & gVal.value();
                if (newVal == 0) {
                    return EdgeFuncHolder(std::make_unique<BottomFunc>());
                } else {
                    return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(newVal));
                }
            }
        } else if (fVal.has_value()) {
            // 0 v Id = 0
            if (fVal.value() == 0)
                return EdgeFuncHolder(std::make_unique<BottomFunc>());
            // T v Id = Id
            else if (typeid(*fn) == typeid(TopFunc))
                return EdgeFuncHolder(std::make_unique<IdFunc>());

            // Id v x = x
            else
                return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(fVal.value()));
        } else if (gVal.has_value()) {
            // Id v 0 = 0
            if (gVal.value() == 0)
                return EdgeFuncHolder(std::make_unique<BottomFunc>());
            // Id v T = Id
            else if (typeid(*other.fn) == typeid(TopFunc))
                return EdgeFuncHolder(std::make_unique<IdFunc>());

            // Id v x = x
            else
                return EdgeFuncHolder(std::make_unique<ActionBitSetFunc>(gVal.value()));
        }
        // Id v Id = Id
        return EdgeFuncHolder(fn->clone());
    }

 private:
    std::unique_ptr<EdgeFunc> fn;
};

const struct EdgeFuncHolder globalIdFunc(std::make_unique<IdFunc>());
const struct EdgeFuncHolder globalBottomFunc(std::make_unique<BottomFunc>());

class EdgeTypeIface {
 public:
    cstring name;
    EdgeType type;
    EdgeFuncHolder fn = globalIdFunc;

    void setFunc(std::unique_ptr<EdgeFunc> f) { fn = EdgeFuncHolder(std::move(f)); }

    size_t apply(size_t i) {
        return (fn)(i);
    }

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
    if (hasFlag(flags, VertexFlags::SO_IDX))       parts.emplace_back("SO_IDX"_cs);
    if (hasFlag(flags, VertexFlags::SO_DATA))      parts.emplace_back("SO_DATA"_cs);

    cstring res;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) res += "|"_cs;
        res += parts[i];
    }
    return res;
}
inline SOFlags operator|(SOFlags a, SOFlags b) {
    return static_cast<SOFlags>(static_cast<unsigned>(a) |
            static_cast<unsigned>(b));
}
// OR-assign
inline SOFlags& operator|=(SOFlags& a, SOFlags b) {
    a = a | b;
    return a;
}
inline bool hasSOFlag(SOFlags v, SOFlags f) {
    return (static_cast<unsigned>(v) & static_cast<unsigned>(f)) != 0;
}
inline cstring soFlagsToString(SOFlags flags) {
    if (flags == SOFlags::NONE) return "NONE"_cs;

    std::vector<cstring> parts;

    if (hasSOFlag(flags, SOFlags::CREATE))  parts.emplace_back("CREATE"_cs);
    if (hasSOFlag(flags, SOFlags::READ))    parts.emplace_back("READ"_cs);
    if (hasSOFlag(flags, SOFlags::UPDATE))  parts.emplace_back("UPDATE"_cs);
    if (hasSOFlag(flags, SOFlags::DELETE))  parts.emplace_back("DELETE"_cs);
    cstring res;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) res += "|"_cs;
        res += parts[i];
    }
    return res;
}

enum class VarVisibility {
    NONE,
    REACHABLE,
    FULL,
};

enum class GenSGMode {
    NONE,
    ON_DEMAND,
    FULL,
};

class Graphs {
 public:
    struct Vertex {
        cstring name;
        VertexFlags flags;
        SOFlags soFlags;
        const IR::Node *node;
        std::vector<const IR::Node *> defVars;
        std::vector<const IR::Node *> useVars;
        cstring color = cstring::empty;
        bool interesting = false;   // used only for VarVertex
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

    // Vertex ID -> Procedure Name
    using ProcOf = hvec_map<vertex_t, cstring>;
    // CALL (caller) -> ENTRY (callee), RETURN (caller)
    using CallMap = hvec_map<vertex_t, std::pair<vertex_t, vertex_t>>;
    // Procedure Name -> list of CALL (caller)
    using ProcCallers = hvec_map<cstring, std::vector<vertex_t>>;

    hvec_map<cstring, ProcOf> procOfs;
    hvec_map<cstring, CallMap> callMaps;
    hvec_map<cstring, ProcCallers> procCallerMaps;

    vertex_t add_vertex(const cstring &name, VertexFlags flags, const IR::Node *node=nullptr);

    void add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                  EdgeType type, std::optional<size_t> actId=std::nullopt);

    void add_edge(const vertex_t &from, const vertex_t &to, const cstring &name,
                  EdgeType type, unsigned cluster_id,
                  std::optional<size_t> actId=std::nullopt);

    vertex_t add_and_connect_vertex(const cstring &name, VertexFlags flags,
                                    const IR::Node *node=nullptr);

    void add_and_connect_vertex(Graphs::vertex_t &target, EdgeType edgeType);

    vertex_t add_var_vertex(const IR::Node *var, std::optional<const vertex_t> node=std::nullopt,
            std::optional<cstring> name=std::nullopt);

    const IR::Node *add_variable_in_vertex(const IR::Node *var,
            const vertex_t &v, bool isUsed,
            std::optional<cstring> name=std::nullopt);

    cstring get_var_name(const IR::Node *var) {
        std::stringstream sstream;
        sstream << var;
        auto fullName = cstring(sstream);
        if (auto *p = fullName.findlast(' ')) return cstring(p + 1);
        return fullName;
    }

    vertex_t get_root_vertex(Graph *g) {
        return *boost::vertices(*g).first;
    }

    class GraphAttributeSetter {
     public:
        void operator()(Graph &g, VarVisibility varVis=VarVisibility::NONE,
                bool showVarEdgeLabel=false) const {
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
                attrs[*vit]["style"_cs] = vertexFlagGetStyle(vinfo, varVis);
                attrs[*vit]["fillcolor"_cs] = vertexFlagGetColor(g, *vit);
                attrs[*vit]["shape"_cs] = vertexFlagGetShape(vinfo.flags);
                attrs[*vit]["width"_cs] = vertexFlagGetWidth(vinfo.flags);
                attrs[*vit]["margin"_cs] = vertexFlagGetMargin();
            }

            auto edges = boost::edges(g);
            for (auto &eit = edges.first; eit != edges.second; ++eit) {
                auto attrs = boost::get(boost::edge_attribute, g);
                auto &ep = g[*eit];
                attrs[*eit]["label"_cs] = edgeTypeGetName(ep, showVarEdgeLabel);
                attrs[*eit]["style"_cs] = edgeTypeGetStyle(g, *eit, varVis);
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
        static cstring vertexFlagGetStyle(const Vertex &vinfo, VarVisibility varVis) {
            if (hasFlag(vinfo.flags, VertexFlags::CONTROL))
                return "dashed"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::EMPTY))
                return "invis"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::CONDITION))
                return "rounded"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::KEY))
                return "rounded"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::SWITCH))
                return "rounded"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::TABLE))
                return "filled"_cs;
            else if (hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
                     hasFlag(vinfo.flags, VertexFlags::SO_IDX) ||
                     hasFlag(vinfo.flags, VertexFlags::SO_DATA))
                return "filled"_cs;
            if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) {
                switch (varVis) {
                    case VarVisibility::NONE:
                        return "invis"_cs;
                    case VarVisibility::FULL:
                        return "filled"_cs;
                    case VarVisibility::REACHABLE:
                        return vinfo.interesting ? "filled"_cs : "invis"_cs;
                    default:
                        break;
                }
                // Unreachable ...
                return "invis"_cs;
            }

            return "solid"_cs;
        }
        cstring vertexFlagGetColor(Graph &g, const vertex_t &v) const {
            const auto &vinfo = g[v];
            if (vinfo.color != cstring::empty) return vinfo.color;

            auto flags = vinfo.flags;
            cstring colorName = cstring::empty;
            if (hasFlag(flags, VertexFlags::TABLE))
                colorName = "lightsalmon"_cs;
            if (hasFlag(flags, VertexFlags::STATEFUL) ||
                    hasFlag(flags, VertexFlags::SO_IDX) ||
                    hasFlag(flags, VertexFlags::SO_DATA))
                colorName = "lightgreen"_cs;
            if (hasFlag(flags, VertexFlags::VARIABLE)) {
                if (vinfo.node == globalNode) return "black"_cs;
                colorName = "white"_cs;
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
        static cstring edgeTypeGetName(EdgeTypeIface &edge, bool showVarEdgeLabel) {
            if (showVarEdgeLabel &&
                    (edge.type == EdgeType::IFDS ||
                    edge.type == EdgeType::IFDS_FT)) {
                return edge.fn.getName();
            }
            return edge.name;
        }
        static cstring edgeTypeGetStyle(Graph &g, const edge_t &ei,
                VarVisibility varVis) {
            bool showVar = false;
            if (varVis == VarVisibility::FULL) showVar = true;
            else if (varVis == VarVisibility::REACHABLE) {
                auto &sinfo = g[boost::source(ei, g)];
                auto &tinfo = g[boost::target(ei, g)];
                if (sinfo.interesting && tinfo.interesting)
                    showVar = true;
            }
            auto &edge = g[ei];
            switch (edge.type) {
                case EdgeType::INTER_PROCEDURE:
                    return "dotted"_cs;
                case EdgeType::CALL_TO_RETURN:
                    return "bold"_cs;
                case EdgeType::DEFUSE:
                    return "dashed"_cs;
                case EdgeType::HAS_VAR:
                    return "invis"_cs;
                case EdgeType::IFDS:
                    return showVar ? "bold"_cs : "invis"_cs;
                case EdgeType::IFDS_FT:
                    return showVar ? cstring::empty : "invis"_cs;
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
    cstring procName;

    // Used by controls.cpp
    std::vector<const IR::Node *> curKeyVars;
    bool storeKeys = false;
    bool setSOData = false;

 public:
    VarVisibility varVis = VarVisibility::NONE;
    GenSGMode genSupergraphs = GenSGMode::NONE;
    static const IR::Node *globalNode;
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_GRAPHS_H_ */
