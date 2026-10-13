#ifndef BACKENDS_IFDS_UTILS_H_
#define BACKENDS_IFDS_UTILS_H_

#include "backends/ifds/graphs.h"
#include "backends/ifds/supergraphs.h"
#include "backends/ifds/tabulation.h"
#include "backends/ifds/ide_pass.h"

namespace P4::IFDS {
    std::vector<Graphs::vertex_t> find_next_cfg_node(Graphs::Graph *g, Graphs::vertex_t v, bool controlOnly=true);
    std::vector<const IR::Node *> find_ret_vars(SuperGraphProp *sgProp, Graphs::vertex_t ret_v);
    std::vector<TabVertex> collect_state_vars_from_dep_edges_dst(Graphs::Graph *g, SuperGraphProp *sgProp,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        const IDEPass::DepEdgeMap &depEdgeMap,
        IDEPass::DepEdgeMap &stateVarMap, bool showLog=false);
    std::vector<TabVertex> collect_state_vars_from_dep_edges_src(Graphs::Graph *g, SuperGraphProp *sgProp,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        const IDEPass::DepEdgeMap &depEdgeMap,
        IDEPass::DepEdgeMap &stateVarMap, bool showLog=false);
    IDEPass::DepEdgeMap convert_dep_edges(const IDEPass::DepEdgeMap &curDepEdges);
    std::vector<TabVertex> collect_state_vars(Graphs::Graph *g, SuperGraphProp *sgProp,
        P4::ReferenceMap *refMap, P4::TypeMap *typeMap, bool showLog=false);

    // Stateful procedures (RegisterAction apply bodies): the register cell is the first INPUT param.
    // Whether @var is the cell or one of its members (e.g. val.field).
    bool is_cell_var(const IR::Node *var, const IR::Node *cell);
    // The cell of the stateful procedure entered at @entry; null when it has no INPUT param.
    const IR::Node *so_proc_cell(Graphs::Graph *g, Graphs::vertex_t entry);
    // Whether some statement of the procedure entered at @entry writes / reads its cell.
    struct CellAccess { bool writes = false; bool reads = false; };
    CellAccess so_proc_cell_access(Graphs::Graph *g, SuperGraphProp *sgProp, Graphs::vertex_t entry);
    // CALL vertices that call the procedure entered at @entry
    std::vector<Graphs::vertex_t> find_call_sites(SuperGraphProp *sgProp, Graphs::vertex_t entry);
}
#endif  /* BACKENDS_IFDS_UTILS_H_ */