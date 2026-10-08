#ifndef BACKENDS_IFDS_EDGE_FUNC_PROVIDER_H_
#define BACKENDS_IFDS_EDGE_FUNC_PROVIDER_H_

#include <optional>

#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"

namespace P4::IFDS {

class Tabulation;

/// IDE edge functions of one analysis instance: decides how a value (the VarBitSet of seed facts
/// reaching a fact) is transformed along an exploded-supergraph edge. Tabulation::init_edge_func
/// consults it before solving; returning std::nullopt leaves the edge's current function (identity
/// after Tabulation::clear_edge_func) in place.
class EdgeFuncProvider {
 public:
    virtual ~EdgeFuncProvider() = default;

    /// Called for every IFDS/IFDS_FT edge @p edge = @p srcVar -> <@p seed> whose target is the
    /// seed fact with bit index @p seedIdx.
    virtual std::optional<EdgeFuncHolder> seed_edge_func(Tabulation &tab, Graphs::vertex_t srcVar,
                                                         const TabVertex &seed, size_t seedIdx,
                                                         const EdgeTypeIface &edge) const = 0;

    /// When true, init_edge_func composes this provider's seed functions onto the edge's current
    /// function instead of replacing it, so seeds sharing one fact all get their bits.
    virtual bool accumulates() const { return false; }

    /// When true, edge_func() is additionally called for every IFDS/IFDS_FT edge of the graph
    /// (after the seed pass), so an instance can attach functions to non-seed facts too.
    virtual bool wants_all_edges() const { return false; }

    /// Edge function for an arbitrary fact edge @p srcVar -> @p dstVar. Only called when
    /// wants_all_edges() is true.
    virtual std::optional<EdgeFuncHolder> edge_func(Tabulation &, Graphs::vertex_t /*srcVar*/,
                                                    Graphs::vertex_t /*dstVar*/,
                                                    const EdgeTypeIface &) const {
        return std::nullopt;
    }
};

/// Generates the seed's bit (i | (1 << seedIdx)) on the edges that define a seed fact:
/// <src, 0> -> <ACTION, var>, <src, 0> -> <STATEFUL & SO_DATA, var>, <exit, non-0> -> <ret, var>,
/// and <0, 0> -> <CFG root, var>. Every other edge stays identity.
class DefaultEdgeFuncProvider : public EdgeFuncProvider {
 public:
    std::optional<EdgeFuncHolder> seed_edge_func(Tabulation &tab, Graphs::vertex_t srcVar,
                                                 const TabVertex &seed, size_t seedIdx,
                                                 const EdgeTypeIface &edge) const override;

 private:
    /// The edge function generating seed @seedIdx's bit.
    static EdgeFuncHolder var_set_func(const Tabulation &tab, size_t seedIdx);
};

const EdgeFuncProvider &defaultEdgeFuncProvider();

}  // namespace P4::IFDS

#endif /* BACKENDS_IFDS_EDGE_FUNC_PROVIDER_H_ */
