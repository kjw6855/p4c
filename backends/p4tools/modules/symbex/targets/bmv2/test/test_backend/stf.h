#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_TEST_BACKEND_STF_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_TEST_BACKEND_STF_H_

#include <gtest/gtest.h>

#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend/stf.h"

namespace P4::P4Tools::Test {

using TestBackendConfiguration = Symbex::TestBackendConfiguration;
using Packet = Symbex::Packet;
using ActionArg = Symbex::ActionArg;
using ActionCall = Symbex::ActionCall;
using Exact = Symbex::Exact;
using Ternary = Symbex::Ternary;
using TableMatch = Symbex::TableMatch;
using TableMatchMap = Symbex::TableMatchMap;
using TableRule = Symbex::TableRule;
using TableConfig = Symbex::TableConfig;
using TestSpec = Symbex::TestSpec;
using STF = Symbex::Bmv2::STF;

/// Helper methods to build configurations for STF Tests.
class STFTest : public testing::Test {
 public:
    TableConfig getForwardTableConfig();
    TableConfig getIPRouteTableConfig();
    TableConfig gettest1TableConfig();
    TableConfig gettest1TableConfig2();
};

}  // namespace P4::P4Tools::Test

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_TEST_BACKEND_STF_H_ */
