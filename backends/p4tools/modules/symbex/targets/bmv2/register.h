#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_REGISTER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_REGISTER_H_

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/target.h"
#include "backends/p4tools/modules/symbex/symbex.h"

namespace P4::P4Tools::Symbex {

/// Register the BMv2 symbex target with the symbex framework.
inline void bmv2RegisterSymbexTarget() { Bmv2::Bmv2V1ModelSymbexTarget::make(); }

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_REGISTER_H_ */
