#ifndef BACKENDS_STATE_DEPENDENCY_P4RUNTIME_TRANSLATION_H_
#define BACKENDS_STATE_DEPENDENCY_P4RUNTIME_TRANSLATION_H_

#include <functional>

#include "frontends/p4/typeMap.h"
#include "ir/ir.h"
#include "ir/visitor.h"

namespace P4::P4StateDependency {

using namespace P4::literals;

/// Verbatim copy of P4Tools::Symbex::Bmv2::PropagateP4RuntimeTranslation
/// (backends/p4tools/modules/symbex/targets/bmv2/p4runtime_translation.{h,cpp}), renamespaced.
///
/// Copied rather than linked: p4c_state_dependency cannot link symbex-lib (symbex-lib already
/// links p4csd-lib, so the dependency would be circular), and referencing the .cpp from under
/// modules/symbex would couple this build to a directory another tool owns. The class has no
/// symbex-specific dependencies -- only TypeMap and the IR -- so the copy is self-contained.
/// Keep it in sync if the symbex original changes; --post-midend's bmv2 parity depends on it.
///
/// Propagates P4Runtime annotations attached to type definitions to the nodes which use these type
/// definitions. For now, this is restricted to key elements and action parameters.
class PropagateP4RuntimeTranslation : public Transform {
    /// We use the typemap to look up the original declaration for type reference.
    /// These declarations may have an annotation.
    std::reference_wrapper<const P4::TypeMap> _typeMap;

    /// Look up annotations relevant to P4Runtime. They may influence the control plane interfaces.
    static std::vector<const IR::Annotation *> lookupP4RuntimeAnnotations(
        const P4::TypeMap &typeMap, const IR::Type *type);

    const IR::Parameter *preorder(IR::Parameter *parameter) override;
    const IR::KeyElement *preorder(IR::KeyElement *keyElement) override;

 public:
    explicit PropagateP4RuntimeTranslation(const P4::TypeMap &typeMap);
};

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_P4RUNTIME_TRANSLATION_H_ */
