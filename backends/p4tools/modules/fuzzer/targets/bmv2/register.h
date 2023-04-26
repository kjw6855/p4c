#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_REGISTER_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_REGISTER_H_

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/modules/fuzzer/options.h"
#include "backends/p4tools/modules/fuzzer/targets/bmv2/bmv2.h"
#include "backends/p4tools/modules/fuzzer/targets/bmv2/target.h"
#include "backends/p4tools/modules/fuzzer/fuzzer.h"

namespace P4Tools {

namespace P4Testgen {

/// Register the BMv2 compiler target with the tools framework.
void bmv2_registerCompilerTarget() { Bmv2::BMv2_V1ModelCompilerTarget::make(); }

/// Register the BMv2 testgen target with the testgen framework.
void bmv2_registerFuzzerTarget() { Bmv2::BMv2_V1ModelTestgenTarget::make(); }

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_TARGETS_BMV2_REGISTER_H_ */
