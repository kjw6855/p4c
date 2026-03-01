#include "non_exact_to_stateful.h"
#include "graphs.h"

namespace P4::P4StateDependency {

void FindNonExactToStateful::analyze_control_graph(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto vertices = boost::vertices(*g);

    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        // find table vertices
        auto &vinfo = (*g)[*vit];

        if (!hasFlag(vinfo.flags, VertexFlags::KEY))
            continue;

        if (vinfo.node == nullptr || !vinfo.node->is<IR::Key>())
            continue;

        auto key = vinfo.node->to<IR::Key>();
        for (auto elVec : key->keyElements) {
            if (elVec->matchType->path->name.name == "exact"_cs)
                continue;

            auto elVar = elVec->to<IR::KeyElement>()->expression;
            // Find NonExact Match Var
            const IR::Node *nonExactVar = nullptr;
            for (auto uv : vinfo.useVars) {
                if (uv->equiv(*elVar)) {
                    nonExactVar = uv;
                    break;
                }
            }
            if (!nonExactVar)
                continue;

            auto nonExactVarIdx = sgProp->varIndexMap[nonExactVar];
            auto nonExactVarVit = sgProp->globalVariables[*vit][nonExactVarIdx];
        }
    }
}

Visitor::profile_t FindNonExactToStateful::init_apply(const IR::Node *n) {
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto *sgProp = (*graphProps)[i];
        auto *tab = new Tabulation{cgg, sgProp};
        tab->init();
        tab->forward_tabulate();

        analyze_control_graph(tab);
    }

    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
