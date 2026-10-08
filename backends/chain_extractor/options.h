#ifndef BACKENDS_CHAIN_EXTRACTOR_OPTIONS_H_
#define BACKENDS_CHAIN_EXTRACTOR_OPTIONS_H_

#include <filesystem>
#include <optional>
#include "backends/ifds/graphs.h"
#include "frontends/common/options.h"

namespace P4::P4ChainExtractor {

using namespace ::P4::IFDS;

class P4ChainExtractorOptions : public CompilerOptions {
 public:
    P4ChainExtractorOptions();
    virtual ~P4ChainExtractorOptions() = default;

    std::filesystem::path graphsDir{"."};
    /// When set, serialize the computed Key + Cond SOChains to this file (JSON) so p4symbex can load
    /// them via --state-dep-cache instead of recomputing the IFDS analysis.
    std::optional<std::string> cacheChainsFile;
    bool loadIRFromJson = false;  // read from json
    bool graphs = true;           // default behavior
    bool fullGraph = false;
    bool jsonOut = false;
    VarVisibility varVis = VarVisibility::NONE;
    GenSGMode genSupergraphs = GenSGMode::NONE;
    /// Opt-in whole-pipeline modeling: analyze Parser->Ingress/Egress as one IFDS supergraph (per
    /// thread) via a synthesized dummy-main, and unroll parser loops in prep. Off by default so the
    /// legacy per-control analysis is byte-identical unless explicitly enabled.
    bool wholePipeline = false;
    /// Opt-in parser-deps mode: compute the parser-state dependency record (header-derived metadata) and
    /// seed those metadata fields as sources of the per-control IFDS, so chains root at parser-derived
    /// metadata. Lightweight alternative to --whole-pipeline; off by default.
    bool parserDeps = false;
    /// Measurement-only mode: build the CFGs, the IFDS supergraphs and the parser graphs, report
    /// their latency ("P4CX.CFG" / "P4CX.Supergraph" / "Parser graphs" in the performance report),
    /// and skip every chain pass plus DOT drawing. Produces no chain counts — do not use it to
    /// generate analysis results.
    bool supergraphOnly = false;
    /// Verification mode: solve {Act, SO} and {Hdr} separately, with the implicit match-key ->
    /// action flow seen by Hdr only, which reproduces p4c_state_dependency's chains exactly. The
    /// default (one solve, implicit flow for every domain) yields a superset of them.
    bool legacyFlow = false;
    /// Opt-in: lower the program with the same midend p4symbex applies (P4Tools::MidEnd default
    /// passes + the per-target deltas + MidEndLast) before analyzing, so latency and chain
    /// positions are measured at p4symbex's IR level instead of near-post-frontend. Off by
    /// default.
    ///
    /// All four relations stay measurable here. That midend runs RemoveActionParameters, but RAP
    /// only rewrites in/inout/out parameters into locals -- directionless ones, i.e. the
    /// control-plane action data the A2S2* chains are rooted at, are kept
    /// (frontends/p4/removeParameters.cpp:122). Verified on Cerberus/multi3: A2S2K is 16 both
    /// with and without this flag.
    bool postMidEnd = false;
    VarEdgeVisibility varEdgeVis = VarEdgeVisibility::NONE;

 private:
    bool isGraphsSet = false;
};

void printPerformanceReport(const std::optional<std::filesystem::path> &basePath = std::nullopt);

}  // namespace P4::P4ChainExtractor

#endif /* BACKENDS_CHAIN_EXTRACTOR_OPTIONS_H_ */
