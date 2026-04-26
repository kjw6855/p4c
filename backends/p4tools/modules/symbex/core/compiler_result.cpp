#include "backends/p4tools/modules/symbex/core/compiler_result.h"

#include <utility>

#include "backends/p4tools/common/compiler/reachability.h"
#include "midend/coverage.h"

namespace P4::P4Tools::Symbex {

SymbexCompilerResult::SymbexCompilerResult(CompilerResult compilerResult,
                                             P4::Coverage::CoverageSet coverableNodes,
                                             const NodesCallGraph *callGraph)
    : CompilerResult(std::move(compilerResult)),
      coverableNodes(std::move(coverableNodes)),
      callGraph(callGraph) {}

const NodesCallGraph &SymbexCompilerResult::getCallGraph() const {
    BUG_CHECK(callGraph != nullptr, "The call graph has not been initialized.");
    return *callGraph;
}

const P4::Coverage::CoverageSet &SymbexCompilerResult::getCoverableNodes() const {
    return coverableNodes;
}

}  // namespace P4::P4Tools::Symbex
