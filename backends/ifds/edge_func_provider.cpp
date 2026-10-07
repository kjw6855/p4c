#include "backends/ifds/edge_func_provider.h"

#include "backends/ifds/tabulation.h"

namespace P4::IFDS {

std::optional<EdgeFuncHolder> DefaultEdgeFuncProvider::seed_edge_func(
        Tabulation &tab, Graphs::vertex_t srcVarIt, const TabVertex &seed, size_t seedIdx,
        const EdgeTypeIface &) const {
    auto *g = tab.g;
    auto *sgProp = tab.sgProp;
    auto &progVarInfo = sgProp->progVarInfo;
    auto &tvinfo = (*g)[seed.node];
    auto srcTv = tab.get_tab_vertex(srcVarIt);

    // <src, 0> -> <act, var>
    if (hasFlag(tvinfo.flags, VertexFlags::ACTION) &&
            progVarInfo[srcTv.node][0] == srcVarIt)
        return var_set_func(tab, seedIdx);
    // <src, 0> -> <STATEFUL & SO_DATA, var>
    if (hasFlag(tvinfo.flags, VertexFlags::STATEFUL) &&
            hasFlag(tvinfo.flags, VertexFlags::SO_DATA) &&
            progVarInfo[srcTv.node][0] == srcVarIt)
        return var_set_func(tab, seedIdx);
    // <src=exit, non-0> -> <ret, var>
    if (hasFlag(tvinfo.flags, VertexFlags::RETURN) &&
            progVarInfo[srcTv.node][0] != srcVarIt)
        return var_set_func(tab, seedIdx);
    // <0, 0> -> <CFGroot, var>
    if (srcVarIt == sgProp->rootVar)
        return var_set_func(tab, seedIdx);
    return std::nullopt;
}

EdgeFuncHolder DefaultEdgeFuncProvider::var_set_func(const Tabulation &tab, size_t seedIdx) {
    return EdgeFuncHolder(std::make_unique<VarSetFunc>(tab.varBitSetSize, seedIdx));
}

const EdgeFuncProvider &defaultEdgeFuncProvider() {
    static const DefaultEdgeFuncProvider provider;
    return provider;
}

}  // namespace P4::IFDS
