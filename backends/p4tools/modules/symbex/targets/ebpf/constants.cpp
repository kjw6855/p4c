#include "backends/p4tools/modules/symbex/targets/ebpf/constants.h"

namespace P4::P4Tools::Symbex::EBPF {

const IR::PathExpression EBPFConstants::ACCEPT_VAR =
    IR::PathExpression(IR::Type_Boolean::get(), new IR::Path("*accept"));

}  // namespace P4::P4Tools::Symbex::EBPF
