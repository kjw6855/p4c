#ifndef BACKENDS_STATE_DEPENDENCY_UTILS_H_
#define BACKENDS_STATE_DEPENDENCY_UTILS_H_

#include "graphs.h"
#include "supergraphs.h"
#include "tabulation.h"
#include "ide_pass.h"

namespace P4::P4StateDependency {
    std::vector<Graphs::vertex_t> find_next_cfg_node(Graphs::Graph *g, Graphs::vertex_t v);
    std::vector<const IR::Node *> find_ret_vars(SuperGraphProp *sgProp, Graphs::vertex_t ret_v);
    std::vector<TabVertex> collect_state_vars_from_dep_edges(Graphs::Graph *g, SuperGraphProp *sgProp,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        const IDEPass::DepEdgeMap &ptsEdgeMap, bool showLog=false);
    std::vector<TabVertex> collect_state_vars(Graphs::Graph *g, SuperGraphProp *sgProp,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap, bool showLog=false);
}
#endif  /* BACKENDS_STATE_DEPENDENCY_UTILS_H_ */