#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_REGISTER_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_REGISTER_H_

#include "backends/p4tools/modules/fuzzer/targets/bmv2/register.h"
#include "backends/p4tools/modules/fuzzer/targets/ebpf/register.h"


namespace P4Tools {

namespace P4Testgen {

void registerCompilerTargets() {
    bmv2_registerCompilerTarget();
    ebpf_registerCompilerTarget();
}

void registerFuzzerTargets() {
    bmv2_registerFuzzerTarget();
    ebpf_registerFuzzerTarget();
}

}  // namespace P4Tools

}  // namespace P4Testgen

#endif  /* BACKENDS_P4TOOLS_MODULES_FUZZER_REGISTER_H_ */
