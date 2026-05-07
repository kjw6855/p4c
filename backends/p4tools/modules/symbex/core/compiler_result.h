#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_COMPILER_RESULT_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_COMPILER_RESULT_H_

#include "backends/p4tools/common/compiler/compiler_result.h"
#include "backends/p4tools/common/compiler/reachability.h"
#include "backends/state_dependency/analysis.h"
#include "midend/coverage.h"

namespace P4::P4Tools::Symbex {

/// Extends the CompilerResult with the associated P4RuntimeApi
class SymbexCompilerResult : public CompilerResult {
 private:
    /// The coverabled Nodes in the analyzed P4 program.
    P4::Coverage::CoverageSet coverableNodes;

    /// The call graph of the analyzed P4 program, if flag --dcg is set.
    const NodesCallGraph *callGraph;

    const P4StateDependency::StateDependencyResult *stateDepResult = nullptr;

 public:
    explicit SymbexCompilerResult(CompilerResult compilerResult,
                                   P4::Coverage::CoverageSet coverableNodes,
                                   const NodesCallGraph *callGraph = nullptr,
                                   const P4StateDependency::StateDependencyResult *stateDepResult = nullptr);

    /// @returns the call graph of the analyzed P4 program, if flag --dcg is set.
    /// If this function is called when the call graph is not set, if will throw an exception.
    /// TODO: Replace this with std::nullopt?
    [[nodiscard]] const NodesCallGraph &getCallGraph() const;

    /// @returns the coverable nodes in the analyzed P4 program.
    [[nodiscard]] const P4::Coverage::CoverageSet &getCoverableNodes() const;

    /// @returns the state dependency analysis result, or nullptr if not computed.
    [[nodiscard]] const P4StateDependency::StateDependencyResult *getStateDep() const;

    DECLARE_TYPEINFO(SymbexCompilerResult, CompilerResult);
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_COMPILER_RESULT_H_ */
