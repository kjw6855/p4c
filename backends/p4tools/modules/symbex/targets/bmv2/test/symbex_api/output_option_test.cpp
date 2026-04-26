#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "test/gtest/helpers.h"

#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test/gtest_utils.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend/protobuf_ir.h"
#include "backends/p4tools/modules/symbex/symbex.h"

namespace P4::P4Tools::Test {

using namespace P4::literals;

class SymbexOutputOptionTest : public SymbexBmv2Test {};

TEST_F(SymbexOutputOptionTest, GenerateOuputsCorrectly) {
    std::stringstream streamTest;
    streamTest << R"p4(
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
control ingress(inout Headers hdr, inout Metadata meta, inout standard_metadata_t sm) {
  apply {
      if (hdr.eth_hdr.dst_addr == 0xDEADDEADDEAD && hdr.eth_hdr.src_addr == 0xBEEFBEEFBEEF && hdr.eth_hdr.ether_type == 0xF00D) {
          mark_to_drop(sm);
      }
  }
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

    auto source = P4_SOURCE(P4Headers::V1MODEL, streamTest.str().c_str());
    auto &symbexOptions = Symbex::SymbexOptions::get();
    symbexOptions.target = "bmv2"_cs;
    symbexOptions.arch = "v1model"_cs;
    symbexOptions.testBackend = "PROTOBUF_IR"_cs;
    symbexOptions.testBaseName = "dummy"_cs;
    symbexOptions.seed = 1;
    symbexOptions.maxTests = 0;
    // Create a bespoke packet for the Ethernet extract call.
    symbexOptions.minPktSize = 112;
    symbexOptions.maxPktSize = 112;

    // In this test we only generate tests with dropped packets.
    {
        symbexOptions.droppedPacketOnly = true;

        auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

        ASSERT_TRUE(testListOpt.has_value());
        const auto &testList = testListOpt.value();
        ASSERT_EQ(testList.size(), 1);
        const auto *protobufIrTest =
            testList[0]->checkedTo<P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>();
        EXPECT_THAT(protobufIrTest->getFormattedTest(),
                    testing::Not(testing::HasSubstr(R"(expected_output_packet)")));
    }

    // Here we flip the option and only generate tests with forwarded packets.
    {
        symbexOptions.droppedPacketOnly = false;
        symbexOptions.outputPacketOnly = true;

        auto testListOpt = Symbex::Symbex::generateTests(source, symbexOptions);

        ASSERT_TRUE(testListOpt.has_value());
        const auto &testList = testListOpt.value();
        ASSERT_EQ(testList.size(), 1);
        const auto *protobufIrTest =
            testList[0]->checkedTo<P4::P4Tools::Symbex::Bmv2::ProtobufIrTest>();
        EXPECT_THAT(protobufIrTest->getFormattedTest(),
                    testing::HasSubstr(R"(expected_output_packet)"));
    }
}

}  // namespace P4::P4Tools::Test
