#include "backends/p4tools/modules/symbex/targets/tofino/compiler_result.h"

#include <utility>

namespace P4::P4Tools::Symbex::Tofino {

TofinoCompilerResult::TofinoCompilerResult(SymbexCompilerResult compilerResult,
                                           DirectExternMap directExternMap)
    : SymbexCompilerResult(std::move(compilerResult)),
      directExternMap(std::move(directExternMap)) {}

const DirectExternMap &TofinoCompilerResult::getDirectExternMap() const { return directExternMap; }

}  // namespace P4::P4Tools::Symbex::Tofino
