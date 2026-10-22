#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_ACTION_PARAM_DOMAINS_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_ACTION_PARAM_DOMAINS_H_

#include <vector>

#include "ir/ir.h"
#include "lib/big_int.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// The values a control plane can install for each action parameter whose P4 type is a
/// serializable enum (`enum bit<3> switch_type_t { TOR = 0, ... }`). The midend's
/// P4::EliminateSerEnums rewrites such a parameter to its underlying bit type before symbolic
/// execution, after which a synthesized argument may take any value of that type -- including ones
/// no member names and no controller can install. The members are therefore recorded from the
/// frontend program, keyed by the action's and the parameter's control-plane names.
class ActionParamDomains {
 public:
    /// Records the enum-typed action parameters of @p program, which must not have been through the
    /// midend yet. Replaces whatever an earlier call recorded.
    static void collect(const IR::P4Program *program);

    /// The declared member values of @p actionName's parameter @p paramName, or nullptr when the
    /// parameter is not enum-typed. Action names match in full or on their last component, the
    /// qualification convention the rest of the control-plane code uses.
    static const std::vector<big_int> *lookup(cstring actionName, cstring paramName);
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_ACTION_PARAM_DOMAINS_H_ */
