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

/// A single write→register→read dependency chain associated with one stateful object.
///
/// Each chain groups the write-side IR nodes (the statements that write header data into
/// the SO) and the read-side IR nodes (the SO itself plus all statements that use its
/// value downstream toward a leaf).  A test path is valid for this chain only when it
/// visits nodes from BOTH sides.
///
/// isSinglePath is always true by construction: the pruned dep graph only retains SO
/// vertices whose value reaches a leaf in the current execution, so every data-write chain
/// has write and read on the same P4 pipeline execution.  Two-path reads (register read
/// from a value written by a prior packet) are represented in the noWriteRead category.
struct DepChain {
    size_t id = 0;
    cstring soName;
    const IR::Node *soNode = nullptr;

    std::unordered_set<const IR::Node *> writeNodes;
    boost::dynamic_bitset<> writeNodeIds;

    std::unordered_set<const IR::Node *> readNodes;
    boost::dynamic_bitset<> readNodeIds;

    bool isSinglePath = true;
};

struct StateDependencyResult {
    /// H2S2V: header variable → stateful object → packet field.
    /// Heap-allocated; caller takes ownership. Null if no deps found.
    DependencyGraphs *h2s2vGraphs = nullptr;

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

    /// IR nodes (IR::Statement, IR::P4Action, etc.) of CFG vertices that participate in
    /// at least one a2s2v or h2s2v dependency chain, populated before CFG cleanup.
    /// Empty means no dependency chains were found in the program.
    /// Used by the symbex test backend to filter test cases when --state-dep is active.
    std::unordered_set<const IR::Node *> depChainNodes;

    /// Stable fallback identity set for dep-chain nodes, indexed by IR::Node::clone_id.
    /// clone_id traces back through any chain of Transform clones to the original node's id,
    /// so it stays consistent even when the target's midend creates independent clones of
    /// the same source nodes.  Used when pointer identity cannot be guaranteed.
    boost::dynamic_bitset<> depChainNodeIds;

    // ---- Per-category node sets (subsets of depChainNodes) ----

    /// Category 1 (--state-dep-read): H2S2V nodes reachable from SO vertices that have
    /// no incoming "write_to" edge (the SO is read without being written in this chain).
    std::unordered_set<const IR::Node *> noWriteReadNodes;
    boost::dynamic_bitset<> noWriteReadNodeIds;

    /// Category 2 (--state-dep-write): flat union of all data-write chain nodes
    /// (H2S2V + H2S2C).  Used for the quick "any chain at all?" guard.
    std::unordered_set<const IR::Node *> dataWriteNodes;
    boost::dynamic_bitset<> dataWriteNodeIds;

    /// Per-register chains for category 2.  A test path is valid only when it visits
    /// nodes from BOTH the write side AND the read side of the same chain.
    std::vector<DepChain> dataWriteChains;

    /// Names of stateful objects (registers) that appear as write targets in category-2 chains.
    /// Used by the test backend to emit register-initialization preambles when
    /// --state-dep-reg-init is set.
    std::unordered_set<cstring> dataWriteSONames;

    /// Category 3 (--state-dep-cond): flat union of all H2S2C data-write chain nodes.
    std::unordered_set<const IR::Node *> dataWriteCondNodes;
    boost::dynamic_bitset<> dataWriteCondNodeIds;

    /// Per-register chains for category 3 (H2S2C only).
    std::vector<DepChain> dataWriteCondChains;
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
