#ifndef BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
#define BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_

#include <filesystem>
#include <unordered_set>

#include <boost/dynamic_bitset.hpp>

#include "backends/state_dependency/act_param_to_stateful.h"
#include "backends/state_dependency/controls.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/state_dependency/hdr_to_stateful.h"
#include "backends/state_dependency/stateful_to_cond.h"
#include "backends/state_dependency/stateful_to_key.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/typeMap.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4StateDependency {

struct StateDependencyResult {
    /// H2S2V: header variable → stateful object → packet field (header/port value sinks).
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *h2s2vGraphs = nullptr;

    /// H2S2K: header variable → stateful object → table match key (key sinks only).
    /// Split out from H2S2V so a field that is both a table key and a written header is
    /// not double-counted. Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *h2s2kGraphs = nullptr;

    /// A2S2V: action parameter → stateful object → packet field.
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *a2s2vGraphs = nullptr;

    /// H2S2C: header variable → stateful object → conditions.
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *h2s2cGraphs = nullptr;

    /// CFG graphs array — non-null only when graphsDir is non-empty (binary mode).
    /// Caller takes ownership. Used by GraphVisitor for full CFG visualization.
    ControlGraphs *cfgGraphs = nullptr;

    /// IFDS checkers — non-null only when graphsDir is non-empty (binary mode).
    /// Caller takes ownership. Used to call set_edge_func() before CFG drawing.
    ActParamToStateful *sdChecker = nullptr;
    HdrToStateful *hdChecker = nullptr;
    StatefulToKey *s2vChecker = nullptr;
    StatefulToCond *h2s2cChecker = nullptr;

    // Chains without write
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> noWriteReadChains;
    // Chains with data write to header value
    // Currently, symbex doesn't generate test case for this chain type
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteHeaderChains;
    // Chains with data write to matche key
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteKeyChains;
    // Chains with data write to condition
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteCondChains;
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

/// Convenience overload: builds its own lightweight midend
/// (TypeChecking → EvaluatorPass → IFDS analysis → RemoveActionParameters → TypeChecking)
/// so callers need not supply a pre-built refMap/typeMap/toplevel.
///
/// The IFDS analysis runs before RemoveActionParameters so A2S2V can still find
/// action-parameter sources.  RemoveActionParameters is applied afterwards so that
/// the dep-chain nodes' clone_ids align with those produced by a target midend that
/// also runs RemoveActionParameters.
///
/// @param isv1  true when compiling P4-14 programs (sets V1 mode on the reference map).
StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  cstring arch,
                                                  bool isv1 = false,
                                                  std::filesystem::path graphsDir = {});

}  // namespace P4::P4StateDependency

#endif  // BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
