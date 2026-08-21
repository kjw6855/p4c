#include "backends/state_dependency/post_midend.h"

#include "backends/bmv2/common/annotations.h"
#include "backends/p4tools/common/compiler/midend.h"
#include "backends/state_dependency/p4runtime_translation.h"
#include "frontends/p4/typeChecking/typeChecker.h"
#include "lib/error.h"
#include "midend/midEndLast.h"

namespace P4::P4StateDependency {

namespace {

/// True when this run targets tofino. Checked before the arch fallback because run_analysis.py
/// passes `--target tofino` for both tofino/tna and tofino/v1model rows, while bmv2 and dpdk rows
/// arrive with an EMPTY --target (only -I/--arch are set) -- so keying on arch alone would send
/// tofino/v1model down the bmv2 path. Mirrors symbex's target registry, where
/// ("tofino","v1model") resolves to Tofino_V1ModelSymbexTarget, a tofino target.
bool isTofino(const CompilerOptions &options) {
    if (options.target.isNullOrEmpty()) return false;
    cstring t = options.target;
    return t == "tofino"_cs || t == "tofino2"_cs;
}

bool isV1Model(const CompilerOptions &options) {
    return !options.arch.isNullOrEmpty() && options.arch == "v1model"_cs;
}

}  // namespace

const IR::P4Program *applyPostMidEnd(const IR::P4Program *program, CompilerOptions &options) {
    // CompilerTarget::runFrontend (backends/p4tools/common/compiler/compiler_target.cpp:78-81)
    // ends with one more round of type checking before handing the program to the midend. The
    // frontend in state_dependency.cpp does not, so do it here to enter the midend in the same
    // shape p4symbex does.
    {
        P4::ReferenceMap feRefMap;
        P4::TypeMap feTypeMap;
        program = program->apply(P4::TypeChecking(&feRefMap, &feTypeMap, true));
        if (program == nullptr || ::P4::errorCount() > 0) return nullptr;
    }

    P4Tools::MidEnd midEnd(options);

    if (isTofino(options)) {
        // AbstractTofinoSymbexTarget::mkMidEnd (symbex/targets/tofino/target.cpp:75-92) delegates
        // to CompilerTarget::mkMidEnd -- which is addDefaultPasses() + setStopOnError(true)
        // (compiler_target.cpp:90-95) -- and then appends RenameKeys. Of RenameKeys only
        // ProcessKeyElems is unconditional; see post_midend.h for why the STF-only half is
        // omitted.
        midEnd.addDefaultPasses();
        midEnd.setStopOnError(true);
        midEnd.addPasses({
            new ProcessKeyElems(),
        });
    } else if (isV1Model(options)) {
        // Bmv2V1ModelSymbexTarget::mkMidEnd (symbex/targets/bmv2/target.cpp:110-123) builds MidEnd
        // directly, prepends three passes, then calls addDefaultPasses(). Note it does NOT call
        // setStopOnError -- unlike every other target, which reaches it via
        // CompilerTarget::mkMidEnd. That asymmetry is reproduced deliberately.
        auto *refMap = midEnd.getRefMap();
        auto *typeMap = midEnd.getTypeMap();
        midEnd.addPasses({
            // Parse BMv2-specific annotations.
            new BMV2::ParseAnnotations(),
            new P4::TypeChecking(refMap, typeMap, true),
            new PropagateP4RuntimeTranslation(*typeMap),
        });
        midEnd.addDefaultPasses();
    } else {
        // pna/dpdk and anything else: symbex builds no target for these (targets/pna and
        // targets/ebpf have no CMakeLists.txt and are never registered), so the base
        // CompilerTarget::mkMidEnd behaviour is the closest defensible equivalent.
        midEnd.addDefaultPasses();
        midEnd.setStopOnError(true);
    }

    // CompilerTarget::runMidEnd (compiler_target.cpp:97-106).
    midEnd.addPasses({
        new P4::MidEndLast(),
    });
    midEnd.addDebugHook(options.getDebugHook(), true);

    program = program->apply(midEnd);
    if (program == nullptr || ::P4::errorCount() > 0) return nullptr;
    return program;
}

}  // namespace P4::P4StateDependency
