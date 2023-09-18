#include "backends/p4tools/modules/testgen/targets/ebpf/ebpf.h"

#include <string>

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/compiler/midend.h"
#include "frontends/common/options.h"

namespace P4Tools::P4Testgen::EBPF {

EBPFCompilerTarget::EBPFCompilerTarget() : CompilerTarget("ebpf", "ebpf") {}

void EBPFCompilerTarget::make() {
    static EBPFCompilerTarget *INSTANCE = nullptr;
    if (INSTANCE == nullptr) {
        INSTANCE = new EBPFCompilerTarget();
    }
}

MidEnd EBPFCompilerTarget::mkMidEnd(const CompilerOptions &options, bool loadIRFromJson) const {
    MidEnd midEnd(options);
    midEnd.addPasses({});
    midEnd.addDefaultPasses(loadIRFromJson);

    return midEnd;
}

}  // namespace P4Tools::P4Testgen::EBPF
