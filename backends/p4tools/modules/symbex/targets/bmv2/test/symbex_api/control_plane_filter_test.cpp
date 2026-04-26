#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>

#include "test/gtest/helpers.h"

#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test/gtest_utils.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend/protobuf_ir.h"
#include "backends/p4tools/modules/symbex/symbex.h"

namespace P4::P4Tools::Test {

using namespace P4::literals;

using testing::Contains;
using testing::HasSubstr;
using testing::Not;

namespace {

class SymbexControlPlaneFilterTest : public SymbexBmv2Test {};

std::string generateTestProgram(const char *ingressBlock) {
    std::stringstream templateString;
    templateString << R"p4(
header ethernet_t {
    bit<48> dst_addr;
    bit<48> src_addr;
    bit<16> ether_type;
}

struct Headers {
  ethernet_t eth_hdr;
}

struct Metadata {  }
parser parse(packet_in pkt, out Headers hdr, inout Metadata m, inout standard_metadata_t sm) {
  state start {
      pkt.extract(hdr.eth_hdr);
      transition accept;
  }
}
control ingress(inout Headers hdr, inout Metadata meta, inout standard_metadata_t sm) {)p4"
                   << ingressBlock << R"p4(
}
control egress(inout Headers hdr, inout Metadata meta, inout standard_metadata_t sm) {
  apply {}
}
control deparse(packet_out pkt, in Headers hdr) {
  apply {
    pkt.emit(hdr.eth_hdr);
  }
}
control verifyChecksum(inout Headers hdr, inout Metadata meta) {
  apply {}
}
control computeChecksum(inout Headers hdr, inout Metadata meta) {
  apply {}
}
V1Switch(parse(), verifyChecksum(), ingress(), egress(), computeChecksum(), deparse()) main;
)p4";
    return P4_SOURCE(P4Headers::V1MODEL, templateString.str().c_str());
}

Symbex::SymbexOptions &generateDefaultApiTestSymbexOptions() {
    auto &symbexOptions = Symbex::SymbexOptions::get();
    symbexOptions.testBackend = "PROTOBUF_IR"_cs;
    symbexOptions.testBaseName = "dummy"_cs;
    symbexOptions.target = "bmv2"_cs;
    symbexOptions.arch = "v1model"_cs;
    symbexOptions.seed = 1;
    symbexOptions.maxTests = 0;
    // Create a bespoke packet for the Ethernet extract call.
    constexpr int kEthHdrBitSize = 112;
    symbexOptions.minPktSize = kEthHdrBitSize;
    symbexOptions.maxPktSize = kEthHdrBitSize;
    return symbexOptions;
}

TEST_F(SymbexControlPlaneFilterTest, GeneratesCorrectTests) {
    auto source = generateTestProgram(R"(
    // Drop the packet.
    action acl_drop() {
        mark_to_drop(sm);
    }

    table drop_table {
        key = {
            hdr.eth_hdr.dst_addr : ternary @name("dst_eth");
        }
        actions = {
            acl_drop();
            @defaultonly NoAction();
        }
    }

    apply {
        if (hdr.eth_hdr.isValid()) {
            drop_table.apply();
        }
    })");
    auto &symbexOptions = generateDefaultApiTestSymbexOptions();

    // First, we ensure that tests are generated correctly. We expect two tests.
    // One which exercises action acl_drop and one which exercises the default action, NoAction.
    auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

    ASSERT_TRUE(testListOpt.has_value());
    const auto &testList = testListOpt.value();
    ASSERT_EQ(testList.size(), 2);
    auto protobufIrTests = P4::P4Tools::Symbex::convertAbstractTestsToConcreteTests<
        P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>(testList);
    // Iterate over each test and convert it to a string.
    // TODO: This should be a Proto.
    std::vector<const char *> protobufIrTestStrings;
    std::transform(protobufIrTests.begin(), protobufIrTests.end(),
                   std::back_inserter(protobufIrTestStrings),
                   [](const P4::P4Tools::Symbex::Bmv2::ProtobufIrTest *test) {
                       return test->getFormattedTest().c_str();
                   });
    // TODO: Add .Times(1) here, but this requires a GoogleTest version update.
    EXPECT_THAT(protobufIrTestStrings, Contains(Not(HasSubstr(R"(expected_output_packet)"))));
    EXPECT_THAT(protobufIrTestStrings, Contains(HasSubstr(R"(expected_output_packet)")));
}

TEST_F(SymbexControlPlaneFilterTest, FiltersControlPlaneEntities) {
    auto source = generateTestProgram(R"(
    // Drop the packet.
    action acl_drop() {
        mark_to_drop(sm);
    }

    table drop_table {
        key = {
            hdr.eth_hdr.dst_addr : ternary @name("dst_eth");
        }
        actions = {
            acl_drop();
            @defaultonly NoAction();
        }
    }

    apply {
        if (hdr.eth_hdr.isValid()) {
            drop_table.apply();
        }
    })");
    auto &symbexOptions = generateDefaultApiTestSymbexOptions();

    // We install a filter.
    // Since we can not generate a config for the table we should only generate one test.
    symbexOptions.skippedControlPlaneEntities = {"ingress.drop_table"_cs};

    auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

    ASSERT_TRUE(testListOpt.has_value());
    const auto &testList = testListOpt.value();
    ASSERT_EQ(testList.size(), 1);
    const auto *test = testList[0];
    const auto *protobufIrTest = test->checkedTo<P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>();
    EXPECT_THAT(protobufIrTest->getFormattedTest(), Not(HasSubstr(R"(expected_packet)")));
}

TEST_F(SymbexControlPlaneFilterTest, IgnoresBogusControlPlaneEntities) {
    auto source = generateTestProgram(R"(
    // Drop the packet.
    action acl_drop() {
        mark_to_drop(sm);
    }

    table drop_table {
        key = {
            hdr.eth_hdr.dst_addr : ternary @name("dst_eth");
        }
        actions = {
            acl_drop();
            @defaultonly NoAction();
        }
    }

    apply {
        if (hdr.eth_hdr.isValid()) {
            drop_table.apply();
        }
    })");
    auto &symbexOptions = generateDefaultApiTestSymbexOptions();

    // This is a bogus control plane element, which is ignored. We expect two tests.
    // One which exercises action acl_drop and one which exercises the default action, NoAction.
    symbexOptions.skippedControlPlaneEntities = {"ingress.bogus_table"_cs};

    auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

    ASSERT_TRUE(testListOpt.has_value());
    const auto &testList = testListOpt.value();
    ASSERT_EQ(testList.size(), 2);
    auto protobufIrTests = P4::P4Tools::Symbex::convertAbstractTestsToConcreteTests<
        P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>(testList);
    // Iterate over each test and convert it to a string.
    // TODO: This should be a Proto.
    std::vector<const char *> protobufIrTestStrings;
    std::transform(protobufIrTests.begin(), protobufIrTests.end(),
                   std::back_inserter(protobufIrTestStrings),
                   [](const P4::P4Tools::Symbex::Bmv2::ProtobufIrTest *test) {
                       return test->getFormattedTest().c_str();
                   });
    // TODO: Add .Times(1) here, but this requires a GoogleTest version update.
    EXPECT_THAT(protobufIrTestStrings, Contains(Not(HasSubstr(R"(expected_output_packet)"))));
    EXPECT_THAT(protobufIrTestStrings, Contains(HasSubstr(R"(expected_output_packet)")));
}

TEST_F(SymbexControlPlaneFilterTest, FiltersMultipleControlPlaneEntities) {
    auto source = generateTestProgram(R"(
    // Drop the packet.
    action acl_drop() {
        mark_to_drop(sm);
    }

    table drop_table {
        key = {
            hdr.eth_hdr.dst_addr : ternary @name("dst_eth");
        }
        actions = {
            acl_drop();
            @defaultonly NoAction();
        }
    }

    // Assign an ether type.
    action assign_eth_type(bit<16> ether_type) {
        hdr.eth_hdr.ether_type = ether_type;
    }

    table set_eth_table {
        key = {
            hdr.eth_hdr.dst_addr : ternary @name("dst_eth");
        }
        actions = {
            assign_eth_type();
            @defaultonly NoAction();
        }
    }

    apply {
        if (hdr.eth_hdr.isValid()) {
            drop_table.apply();
            set_eth_table.apply();
        }
    })");
    auto &symbexOptions = generateDefaultApiTestSymbexOptions();

    // We install a filter.
    // Since we can not generate a config for the table we should only generate one test.
    symbexOptions.skippedControlPlaneEntities = {"ingress.drop_table"_cs,
                                                  "ingress.set_eth_table"_cs};

    auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

    ASSERT_TRUE(testListOpt.has_value());
    const auto &testList = testListOpt.value();
    ASSERT_EQ(testList.size(), 1);
    const auto *test = testList[0];
    const auto *protobufIrTest = test->checkedTo<P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>();
    EXPECT_THAT(protobufIrTest->getFormattedTest(), Not(HasSubstr(R"(expected_packet)")));
}

}  // namespace

}  // namespace P4::P4Tools::Test
