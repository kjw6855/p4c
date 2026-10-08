#include "backends/ifds/domain.h"

#include "backends/ifds/tabulation.h"

namespace P4::IFDS {

std::optional<size_t> DomainSet::index_of(cstring name) const {
    for (size_t d = 0; d < domains.size(); d++)
        if (domains[d]->name() == name) return d;
    return std::nullopt;
}

void DomainSet::layout(Tabulation *tab) {
    allSeeds.clear();
    ranges.clear();
    for (auto *dom : domains) {
        auto begin = allSeeds.size();
        auto seeds = dom->seeds(tab);
        allSeeds.insert(allSeeds.end(), seeds.begin(), seeds.end());
        ranges.emplace_back(begin, allSeeds.size());
    }
}

std::vector<TabVertex> DomainSet::seeds_of(size_t d) const {
    auto [begin, end] = range(d);
    return {allSeeds.begin() + begin, allSeeds.begin() + end};
}

size_t DomainSet::domain_of(size_t seedIdx) const {
    for (size_t d = 0; d < ranges.size(); d++)
        if (seedIdx >= ranges[d].first && seedIdx < ranges[d].second) return d;
    BUG("Seed index %1% is out of every domain range", seedIdx);
}

VarBitSet DomainSet::mask(size_t d, size_t width) const {
    VarBitSet m(width);
    auto [begin, end] = range(d);
    for (size_t i = begin; i < end && i < width; i++) m.set(i);
    return m;
}

std::vector<TabVertex> DomainSet::targets(const Tabulation &tab, const VarBitSet &value,
                                          size_t d) const {
    if (value.none() || value == tab.topEnvValue) return {};
    std::vector<TabVertex> found;
    auto [begin, end] = range(d);
    for (size_t pos = begin; pos < end && pos < value.size(); pos++)
        if (value.test(pos)) found.push_back(tab.targetVars[pos]);
    return found;
}

std::optional<EdgeFuncHolder> CompositeEdgeFuncProvider::seed_edge_func(
        Tabulation &tab, Graphs::vertex_t srcVar, const TabVertex &seed, size_t seedIdx,
        const EdgeTypeIface &edge) const {
    return domains.domain(domains.domain_of(seedIdx))->edge_funcs()
            .seed_edge_func(tab, srcVar, seed, seedIdx, edge);
}

}  // namespace P4::IFDS
