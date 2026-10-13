#ifndef BACKENDS_CHAIN_EXTRACTOR_ANALYSIS_H_
#define BACKENDS_CHAIN_EXTRACTOR_ANALYSIS_H_

#include <filesystem>
#include <map>
#include <set>
#include <unordered_set>

#include <boost/dynamic_bitset.hpp>

#include "backends/chain_extractor/chain_pass.h"
#include "backends/ifds/controls.h"
#include "backends/chain_extractor/dependency_graph.h"
#include "backends/chain_extractor/parser_deps.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/typeMap.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4ChainExtractor {

using namespace ::P4::IFDS;

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

    /// The chain pass — non-null only when graphsDir is non-empty (binary mode).
    /// Caller takes ownership. Used to call set_edge_func() before CFG drawing.
    FindChains *chains = nullptr;

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
    // Chains whose sink decides the packet's output (egress port, drop, multicast group):
    // *2S2O end at an output variable (data flow), *2S2OutC at a condition guarding an output write,
    // *2S2OutK at the match key of a table where some action writes an output.
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteOutChains;       // H2S2O
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteOutCondChains;   // H2S2OutC
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteA2SOutChains;    // A2S2O
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteA2SOutCondChains;// A2S2OutC
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteOutKeyChains;    // H2S2OutK
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> dataWriteA2SOutKeyChains; // A2S2OutK

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

/// Bitmask of chain categories to compute. The expensive IFDS sink passes are gated on these, so a
/// caller that needs only one category skips the rest. Each of the two *base* passes runs whenever
/// any category derived from it is requested: HDR->SO for SD_KEY/SD_HEADER/SD_COND, ACT->SO for
/// SD_A2S2V/SD_A2S2K/SD_A2S2C. (The ACT->SO rule used to key on SD_A2S2V alone, which silently
/// zeroed A2S2K/A2S2C for a caller that asked for those without SD_A2S2V -- they consume the
/// stateVars/depEdgeMaps that base pass populates.)
///
/// Symbex's Tampering policy needs only SD_KEY; AlteringPath needs only SD_COND. The mask is
/// honoured in BOTH library and graph/binary mode: binary mode used to force SD_ALL, which made it
/// impossible for the p4c_chain_extractor binary to request its four-type measurement set.
enum SDCategories : unsigned {
    SD_A2S2V = 1u,
    SD_KEY = 2u,
    SD_HEADER = 4u,
    SD_COND = 8u,
    SD_A2S2K = 16u,   // action parameter -> stateful object -> table match key
    SD_A2S2C = 32u,   // action parameter -> stateful object -> condition
    SD_H2S2O = 64u,       // header -> stateful object -> output variable
    SD_H2S2OUTC = 128u,   // header -> stateful object -> condition guarding an output write
    SD_A2S2O = 256u,      // action parameter -> stateful object -> output variable
    SD_A2S2OUTC = 512u,   // action parameter -> stateful object -> output-guarding condition
    SD_H2S2OUTK = 1024u,  // header -> stateful object -> key of an output-writing table
    SD_A2S2OUTK = 2048u,  // action parameter -> stateful object -> key of an output-writing table
    /// Every relation whose sink decides the packet's output.
    SD_OUTPUT = SD_H2S2O | SD_H2S2OUTC | SD_A2S2O | SD_A2S2OUTC | SD_H2S2OUTK | SD_A2S2OUTK,
    SD_ALL = SD_A2S2V | SD_KEY | SD_HEADER | SD_COND | SD_A2S2K | SD_A2S2C | SD_OUTPUT,
    /// Exactly the categories the SOChain cache format can carry: chain_cache.cpp serializes
    /// H2S2K/H2S2C as "key"/"cond", A2S2K/A2S2C as "a2s2k"/"a2s2c", and the output relations as
    /// "h2s2o"/"h2s2outc"/"h2s2outk"/"a2s2o"/"a2s2outc"/"a2s2outk". A cache builder that computes more than this is
    /// doing work it will discard.
    SD_CACHEABLE = SD_KEY | SD_COND | SD_A2S2K | SD_A2S2C | SD_OUTPUT,
    /// The four source->SO->sink relations the p4c_chain_extractor binary measures and reports:
    /// H2S2K (SD_KEY), H2S2C (SD_COND), A2S2K, A2S2C. The two value-sink families -- H2S2V
    /// (SD_HEADER) and A2S2V -- are deliberately excluded; nothing downstream consumes them.
    SD_FOUR = SD_KEY | SD_COND | SD_A2S2K | SD_A2S2C,
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
///                  (timers "P4CX.CFG" / "P4CX.Supergraph"); never read counts from such a result.
/// @param legacyFlow Solve {Act, SO} and {Hdr} separately, with the implicit match-key -> action
///                  flow seen by Hdr only. Reproduces p4c_state_dependency exactly; the default
///                  (one solve, implicit flow for every domain) yields a superset of its chains.
StateDependencyResult runStateDependencyAnalysis(const IR::P4Program *program,
                                                  P4::ReferenceMap *refMap,
                                                  P4::TypeMap *typeMap,
                                                  const IR::ToplevelBlock *toplevel,
                                                  cstring arch,
                                                  std::filesystem::path graphsDir = {},
                                                  unsigned categories = SD_ALL,
                                                  bool wholePipeline = false,
                                                  bool parserDeps = false,
                                                  bool supergraphOnly = false,
                                                  bool legacyFlow = false);

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
                                                  bool supergraphOnly = false,
                                                  bool legacyFlow = false);

/// Chains of one output relation (H2S2O, H2S2OutC, H2S2OutK or their A2S counterparts) into @out:
/// the first hop (A2S when @fromAct, else H2S) plus the register reads reaching the relation's
/// sinks (@sinkMap), assembled per graph of @graphs and pruned like A2S2K. @parserDeps (null when
/// --parser-deps is off) pins the H2S chains; a non-empty @graphsDir exports "<graph>_<name>_dep.dot".
void assemble_output_chains(const std::vector<Graph *> &graphs, const FindChains &chains,
                            bool fromAct, IDEPass::DepEdgeMap GraphChainMaps::*sinkMap,
                            const char *name, const std::filesystem::path &graphsDir,
                            const ParserDepsRecord *parserDeps,
                            std::map<cstring, std::vector<DependencyGraphs::SOChain>> &out);

/// Hop annotation of the OutK / OutC chains of @byGraph: the tables whose action selection steers
/// a chain's register towards the sink (a read of the register reaches their key, and one of their
/// actions reaches the sink), in CFG order; an OutK chain ends with its sink table.
void annotate_hops(const FindChains &chains,
                   std::map<cstring, std::vector<DependencyGraphs::SOChain>> &byGraph);

/// Append the action param @info accessed at @at to @list, unless already listed.
void add_cp_source(std::vector<DependencyGraphs::SOChain::CPSource> &list,
                   const GraphChainMaps::CPSourceInfo &info, const IR::Node *at);

/// Cross-plane roles of the A2S chains of @byGraph (SOChain::cp): the action params reaching the
/// write's data / index and the read's index, named by the chain pass. An add-on-miss table is
/// read at its match key, so its readIdx is the params reaching that key.
void annotate_cp(const FindChains &chains,
                 std::map<cstring, std::vector<DependencyGraphs::SOChain>> &byGraph);

}  // namespace P4::P4ChainExtractor

#endif  // BACKENDS_CHAIN_EXTRACTOR_ANALYSIS_H_
