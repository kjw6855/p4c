#include "backends/p4tools/modules/testgen/targets/tna/tna.h"

#include <string>

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/compiler/midend.h"
#include "control-plane/addMissingIds.h"
#include "control-plane/p4RuntimeArchStandard.h"
#include "frontends/common/options.h"
#include "frontends/common/resolveReferences/resolveReferences.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/testgen/options.h"

namespace P4Tools::P4Testgen::Tna {

TnaCompilerTarget::TnaCompilerTarget() : CompilerTarget("tna", "v1model") {}

void TnaCompilerTarget::make() {
    static TnaCompilerTarget *INSTANCE = nullptr;
    if (INSTANCE == nullptr) {
        INSTANCE = new TnaCompilerTarget();
    }
}

MidEnd TnaCompilerTarget::mkMidEnd(const CompilerOptions &options, bool loadIRFromJson) const {
    MidEnd midEnd(options);
    auto *refMap = midEnd.getRefMap();
    auto *typeMap = midEnd.getTypeMap();

    if (loadIRFromJson) {
        midEnd.addPasses({
            new P4::ResolveReferences(refMap),
        });
    }

    midEnd.addPasses({
        // Parse TNA-specific annotations.
        // TODO: Handle p4runtime_translation and controller_header
        new P4::ParseAnnotations("TNA", false,
                               {PARSE_EMPTY("metadata"), PARSE_EXPRESSION_LIST("field_list"),
                                PARSE("alias", StringLiteral), PARSE("priority", Constant)}),
        // Parse P4Runtime-specific annotations and insert missing IDs.
        // Only do this for the protobuf back end.
        TestgenOptions::get().testBackend == "PROTOBUF"
        ? new P4::AddMissingIdAnnotations(
                refMap, typeMap, new P4::ControlPlaneAPI::Standard::V1ModelArchHandlerBuilder())
        : nullptr,
    });

    midEnd.addDefaultPasses(loadIRFromJson);

    return midEnd;
}

}  // namespace P4Tools::P4Testgen::Tna
