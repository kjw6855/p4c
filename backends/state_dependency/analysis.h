#ifndef BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
#define BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_

#include <filesystem>

#include "backends/state_dependency/act_param_to_stateful.h"
#include "backends/state_dependency/controls.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/state_dependency/hdr_to_stateful.h"
#include "backends/state_dependency/stateful_to_key.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/typeMap.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4StateDependency {

struct StateDependencyResult {
    /// H2S2V: header variable → stateful object → packet field.
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *h2s2vGraphs = nullptr;

    /// A2S2V: action parameter → stateful object → packet field.
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *a2s2vGraphs = nullptr;

    /// CFG graphs array — non-null only when graphsDir is non-empty (binary mode).
    /// Caller takes ownership. Used by GraphVisitor for full CFG visualization.
    ControlGraphs *cfgGraphs = nullptr;

    /// IFDS checkers — non-null only when graphsDir is non-empty (binary mode).
    /// Caller takes ownership. Used to call set_edge_func() before CFG drawing.
    ActParamToStateful *sdChecker = nullptr;
    HdrToStateful *hdChecker = nullptr;
    StatefulToKey *s2vChecker = nullptr;
};

/// Run the full state-dependency analysis (A2S2V and H2S2V) on a compiled P4 program.
///
/// All IR::Node* values stored in the returned DependencyGraphs are drawn from @program,
/// so pointer comparisons against @program's IR nodes are valid.
///
/// @param program   The IR to analyze. Must be the same instance the caller's executor
///                  will operate on (e.g. post-p4tools-midend) for pointer identity to hold.
/// @param refMap    Reference map built from @program.
/// @param typeMap   Type map built from @program.
/// @param toplevel  ToplevelBlock obtained by evaluating @program.
/// @param arch      Architecture name (e.g. "v1model", "tna").
/// @param graphsDir If non-empty, export intermediate and final dep-graphs as DOT files
///                  into this directory (full/merged/pruned variants). Pass {} to skip.
StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  P4::ReferenceMap *refMap,
                                                  P4::TypeMap *typeMap,
                                                  const IR::ToplevelBlock *toplevel,
                                                  cstring arch,
                                                  std::filesystem::path graphsDir = {});

}  // namespace P4::P4StateDependency

#endif  // BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
