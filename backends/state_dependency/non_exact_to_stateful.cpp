#include "non_exact_to_stateful.h"
#include "graphs.h"

namespace P4::P4StateDependency {

void FindNonExactToStateful::analyze_control_graph(Graph *g, SuperGraphProp &sgProp) {
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

            // Found NonExact Match
            // Check if it's accessible from Root
        }
    }
}

Visitor::profile_t FindNonExactToStateful::init_apply(const IR::Node *n) {
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        auto *cgg = (*controlGraphsArray)[i];
        auto &sgProp = (*graphProps)[i];
        analyze_control_graph(cgg, sgProp);
    }
    return (this->Inspector::init_apply(n));
}

}  // namespace P4::P4StateDependency
