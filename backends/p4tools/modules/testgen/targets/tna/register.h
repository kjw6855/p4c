#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_REGISTER_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_REGISTER_H_

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/targets/tna/tna.h"
#include "backends/p4tools/modules/testgen/targets/tna/target.h"
#include "backends/p4tools/modules/testgen/testgen.h"

namespace P4Tools::P4Testgen {

/// Register the TNA compiler target with the tools framework.
inline void tnaRegisterCompilerTarget() { Tna::TnaCompilerTarget::make(); }

/// Register the TNA testgen target with the testgen framework.
inline void tnaRegisterTestgenTarget() { Tna::TnaTestgenTarget::make(); }

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_TNA_REGISTER_H_ */
