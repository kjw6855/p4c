#ifndef BACKENDS_IFDS_DOMAIN_H_
#define BACKENDS_IFDS_DOMAIN_H_

#include <utility>
#include <vector>

#include "backends/ifds/edge_func_provider.h"
#include "backends/ifds/flow_overlay.h"
#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"

namespace P4::IFDS {

class Tabulation;

/// One fact family of a multi-domain IDE solve (e.g. action params, headers, register reads).
/// Every seed of a domain owns one bit of the shared VarBitSet value; the domain decides where
/// its seeds are, which extra fact edges it needs, and how its bits are generated.
class IDEDomain {
 public:
    virtual ~IDEDomain() = default;

    virtual cstring name() const = 0;
    /// Seed facts of this domain in tab's graph.
    virtual std::vector<TabVertex> seeds(Tabulation *tab) = 0;
    /// Extra fact edges this domain needs. They are shared by every domain of the solve
    /// (edge functions are gen-only), and must go through @overlay so they can be restored.
    virtual void add_flow_functions(Tabulation *, const std::vector<TabVertex> &/*seeds*/,
                                    FlowOverlay &/*overlay*/) {}
    /// How this domain's seed bits are generated (see EdgeFuncProvider::seed_edge_func).
    virtual const EdgeFuncProvider &edge_funcs() const { return defaultEdgeFuncProvider(); }
};

/// An ordered set of domains solved together. Registration order is the bit layout:
/// [ d0 seeds | d1 seeds | ... | reserved ]; domain d owns the global seed indices range(d).
class DomainSet {
 public:
    DomainSet() = default;
    explicit DomainSet(std::vector<IDEDomain *> domains) : domains(std::move(domains)) {}

    void add(IDEDomain *d) { domains.push_back(d); }
    size_t size() const { return domains.size(); }
    IDEDomain *domain(size_t d) const { return domains.at(d); }
    /// Index of the domain named @name, if registered.
    std::optional<size_t> index_of(cstring name) const;

    /// Collect every domain's seeds for tab's graph and lay out the global seed list.
    void layout(Tabulation *tab);

    /// Global seed list (concatenation of every domain's seeds, in registration order).
    const std::vector<TabVertex> &seeds() const { return allSeeds; }
    /// This domain's seeds (a slice of seeds()).
    std::vector<TabVertex> seeds_of(size_t d) const;
    /// Global seed index range [begin, end) owned by domain d.
    std::pair<size_t, size_t> range(size_t d) const { return ranges.at(d); }
    /// Domain owning global seed index @seedIdx.
    size_t domain_of(size_t seedIdx) const;
    /// Bits of domain d in a value of @width bits.
    VarBitSet mask(size_t d, size_t width) const;
    /// Seeds of domain d present in @value, in bit order. Empty for an unset or top value.
    std::vector<TabVertex> targets(const Tabulation &tab, const VarBitSet &value, size_t d) const;

 private:
    std::vector<IDEDomain *> domains;
    std::vector<TabVertex> allSeeds;
    std::vector<std::pair<size_t, size_t>> ranges;
};

/// Sends each seed's edges to the provider of the domain owning it. Several seeds can share one
/// fact (overlapping domains), so this provider accumulates: init_edge_func composes its gens.
class CompositeEdgeFuncProvider : public EdgeFuncProvider {
 public:
    explicit CompositeEdgeFuncProvider(const DomainSet &domains) : domains(domains) {}

    std::optional<EdgeFuncHolder> seed_edge_func(Tabulation &tab, Graphs::vertex_t srcVar,
                                                 const TabVertex &seed, size_t seedIdx,
                                                 const EdgeTypeIface &edge) const override;
    bool accumulates() const override { return true; }

 private:
    const DomainSet &domains;
};

}  // namespace P4::IFDS

#endif /* BACKENDS_IFDS_DOMAIN_H_ */
