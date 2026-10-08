#ifndef BACKENDS_CHAIN_EXTRACTOR_POST_MIDEND_H_
#define BACKENDS_CHAIN_EXTRACTOR_POST_MIDEND_H_

#include <regex>  // NOLINT

#include "frontends/common/options.h"
#include "ir/ir.h"
#include "ir/pass_manager.h"
#include "lib/cstring.h"

namespace P4::P4ChainExtractor {

using namespace P4::literals;

/// Verbatim copy of P4Tools::Symbex::Tofino::ProcessKeyElems
/// (backends/p4tools/modules/symbex/targets/tofino/rename_keys.h), renamespaced.
///
/// Copied rather than included: the enclosing RenameKeys PassManager reads
/// SymbexOptions::get().testBackend in its constructor, and linking symbex's options into
/// p4c_chain_extractor would be circular (symbex-lib already links p4ccx-lib). ProcessKeyElems
/// itself has no such dependency.
///
/// RenameKeys also appends ProcessAnnotatedTables, but only when testBackend == "STF". Every
/// measurement run uses --test-backend protobuf, so that pass never fires for the IR we are
/// reproducing and is deliberately omitted here.
///
/// 1. prunes all valid key matches with trailing '$'.
/// 2. replaces header stack indices hdr[<index>] with hdr$<index>.
class ProcessKeyElems : public Transform {
 public:
    ProcessKeyElems() { setName("ProcessKeyElems"); }

    IR::Node *postorder(IR::KeyElement *keyElem) override {
        const auto *nameAnnot = keyElem->getAnnotation(IR::Annotation::nameAnnotation);
        CHECK_NULL(nameAnnot);
        cstring keyName = nameAnnot->getName();

        auto isReplaced = false;

        const auto *foundValid = keyName.find(".$valid");
        if (foundValid != nullptr) {
            // Remove the trailing '$'.
            keyName = keyName.exceptLast(1);
            isReplaced = true;
        }

        // Replace header stack indices hdr[<index>] with hdr$<index>. This
        // is output in the context.json
        std::regex hdrStackRegex(R"(\[([0-9]+)\])");
        auto indexName = std::regex_replace(keyName.c_str(), hdrStackRegex, "$$$1");
        if (indexName != keyName.c_str()) {
            keyName = cstring::to_cstring(indexName);
            isReplaced = true;
        }

        if (isReplaced) {
            ::P4::warning("Replacing invalid key %1% with %2% for key %3%. ", keyName, keyName,
                          keyElem);
            // Update the key's annotation with the new fieldName.
            keyElem->addOrReplaceAnnotation(IR::Annotation::nameAnnotation,
                                            new IR::StringLiteral(keyName));
            return keyElem;
        }

        return keyElem;
    }
};

/// Lower @p program with the same midend p4symbex applies, so the state-dependency analysis runs
/// on the IR level p4symbex analyzes (see --post-midend). Returns the lowered program, or nullptr
/// if the midend errored out.
///
/// The pass list itself is P4Tools::MidEnd (backends/p4tools/common/compiler/midend.cpp), reused
/// rather than copied so it cannot drift; only the small per-target deltas that live inside
/// modules/symbex are reproduced here.
const IR::P4Program *applyPostMidEnd(const IR::P4Program *program, CompilerOptions &options);

}  // namespace P4::P4ChainExtractor

#endif /* BACKENDS_CHAIN_EXTRACTOR_POST_MIDEND_H_ */
