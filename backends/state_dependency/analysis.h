#ifndef BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
#define BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_

#include <filesystem>
#include <map>
#include <set>
#include <unordered_set>

#include <boost/dynamic_bitset.hpp>

#include "backends/state_dependency/act_param_to_stateful.h"
#include "backends/state_dependency/controls.h"
#include "backends/state_dependency/dependency_graph.h"
#include "backends/state_dependency/hdr_to_stateful.h"
#include "backends/state_dependency/parser_deps.h"
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
    // A2S2K: action parameter -> stateful object -> table match key
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteA2SKeyChains;
    // A2S2C: action parameter -> stateful object -> condition
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteA2SCondChains;

    // Distinct stateful-object (register) NAME sets, populated only in binary mode
    // (graphsDir set) for the metrics report. First-hop sets (regA2S/regH2S) are the SOs
    // written by an action param / header; the *2S2[KC] sets are the SOs lying on a chain to
    // that sink; allStatefulObjects is every STATEFUL vertex (the D_all denominator).
    std::set<cstring> regA2S;
    std::set<cstring> regH2S;
    std::set<cstring> regA2S2K;
    std::set<cstring> regA2S2C;
    std::set<cstring> regH2S2K;
    std::set<cstring> regH2S2C;
    std::set<cstring> allStatefulObjects;
    // Declared stateful-object instances -> extern type (Register/Counter/Meter/AddOnMiss), keyed
    // by controlPlaneName() so the names match the dep-graph SO names. This is the authoritative
    // D_all/D_crao/D_rao denominator (a whole-program visitor, independent of any dep chain).
    std::map<cstring, cstring> soTypeByName;

    // --parser-deps: merged parser-state dependency record (header-derived metadata -> header fields),
    // used to seed sources and (in p4symbex) pin header values per phase. Empty otherwise.
    ParserDepsRecord parserDepsRecord;
};

/// Bitmask of chain categories to compute. The four expensive IFDS sink passes are gated on
/// these, so a caller that needs only one category skips the rest. The base HDR->SO pass always
/// runs when any H2S2 category (KEY/HEADER/COND) is requested. Symbex's Tampering policy needs
/// only SD_KEY; AlteringPath needs only SD_COND. SD_ALL preserves the full analysis (graph/binary
/// mode, where graphsDir is set, always computes everything regardless of this mask).
enum SDCategories : unsigned {
    SD_A2S2V = 1u,
    SD_KEY = 2u,
    SD_HEADER = 4u,
    SD_COND = 8u,
    SD_A2S2K = 16u,   // action parameter -> stateful object -> table match key
    SD_A2S2C = 32u,   // action parameter -> stateful object -> condition
    SD_ALL = SD_A2S2V | SD_KEY | SD_HEADER | SD_COND | SD_A2S2K | SD_A2S2C,
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
/// @param supergraphOnly Stop after CFG + IFDS supergraph generation and return a result whose
///                  chain containers are all empty. Only for latency measurement of that stage
///                  (timers "P4SD.CFG" / "P4SD.Supergraph"); never read counts from such a result.
StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  P4::ReferenceMap *refMap,
                                                  P4::TypeMap *typeMap,
                                                  const IR::ToplevelBlock *toplevel,
                                                  cstring arch,
                                                  std::filesystem::path graphsDir = {},
                                                  unsigned categories = SD_ALL,
                                                  bool wholePipeline = false,
                                                  bool parserDeps = false,
                                                  bool supergraphOnly = false);

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
                                                  std::filesystem::path graphsDir = {},
                                                  unsigned categories = SD_ALL,
                                                  bool wholePipeline = false,
                                                  bool parserDeps = false,
                                                  bool supergraphOnly = false);

}  // namespace P4::P4StateDependency

#endif  // BACKENDS_STATE_DEPENDENCY_ANALYSIS_H_
