#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_COMPILER_RESULT_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_COMPILER_RESULT_H_

#include "backends/p4tools/modules/symbex/core/compiler_result.h"
#include "backends/p4tools/modules/symbex/targets/tofino/map_direct_externs.h"

namespace P4::P4Tools::Symbex::Tofino {

/// Extends the CompilerResult with information specific to the V1Model running on BMv2.
class TofinoCompilerResult : public SymbexCompilerResult {
 private:
    /// The map of direct extern declarations which are attached to a table.
    DirectExternMap directExternMap;

 public:
    explicit TofinoCompilerResult(SymbexCompilerResult compilerResult,
                                  DirectExternMap directExternMap);

    /// @returns the map of direct extern declarations which are attached to a table.
    [[nodiscard]] const DirectExternMap &getDirectExternMap() const;

    DECLARE_TYPEINFO(TofinoCompilerResult, SymbexCompilerResult);
};

}  // namespace P4::P4Tools::Symbex::Tofino

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_COMPILER_RESULT_H_ */
