#ifndef BACKENDS_STATE_DEPENDENCY_OPTIONS_H_
#define BACKENDS_STATE_DEPENDENCY_OPTIONS_H_

#include <filesystem>
#include <optional>
#include "backends/state_dependency/graphs.h"
#include "frontends/common/options.h"

namespace P4::P4StateDependency {

class P4StateDependencyOptions : public CompilerOptions {
 public:
    P4StateDependencyOptions();
    virtual ~P4StateDependencyOptions() = default;

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
    /// their latency ("P4SD.CFG" / "P4SD.Supergraph" / "Parser graphs" in the performance report),
    /// and skip every chain pass plus DOT drawing. Produces no chain counts — do not use it to
    /// generate analysis results.
    bool supergraphOnly = false;
    VarEdgeVisibility varEdgeVis = VarEdgeVisibility::NONE;

 private:
    bool isGraphsSet = false;
};

void printPerformanceReport(const std::optional<std::filesystem::path> &basePath = std::nullopt);

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_OPTIONS_H_ */
