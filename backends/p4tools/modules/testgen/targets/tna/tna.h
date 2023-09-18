#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TNA_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TNA_H_

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/compiler/midend.h"
#include "frontends/common/options.h"

namespace P4Tools::P4Testgen::Tna {

class TnaCompilerTarget : public CompilerTarget {
 public:
    /// Registers this target.
    static void make();

 private:
    MidEnd mkMidEnd(const CompilerOptions &options, bool loadIRFromJson) const override;

    TnaCompilerTarget();
};

}  // namespace P4Tools::P4Testgen::Tna

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_TNA_H_ */
