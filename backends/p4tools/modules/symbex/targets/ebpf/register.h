#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_EBPF_REGISTER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_EBPF_REGISTER_H_

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/ebpf/target.h"
#include "backends/p4tools/modules/symbex/symbex.h"

namespace P4::P4Tools::Symbex {

/// Register the ebpf symbex target with the symbex framework.
inline void ebpfRegisterSymbexTarget() { EBPF::EBPFSymbexTarget::make(); }

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_EBPF_REGISTER_H_ */
