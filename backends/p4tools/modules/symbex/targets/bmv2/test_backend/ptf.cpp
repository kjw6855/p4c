#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend/ptf.h"

#include <filesystem>
#include <iomanip>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <inja/inja.hpp>

#include "backends/p4tools/common/lib/format_int.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "lib/exceptions.h"
#include "lib/log.h"
#include "nlohmann/json.hpp"

#include "backends/p4tools/modules/symbex/lib/tamper_kind.h"

namespace P4::P4Tools::Symbex::Bmv2 {

PTF::PTF(const TestBackendConfiguration &testBackendConfiguration)
    : Bmv2TestFramework(testBackendConfiguration) {}

std::vector<std::pair<size_t, size_t>> PTF::getIgnoreMasks(const IR::Constant *mask) {
    std::vector<std::pair<size_t, size_t>> ignoreMasks;
    if (mask == nullptr) {
        return ignoreMasks;
    }
    if (mask->type->width_bits() == 0) {
        return ignoreMasks;
    }
    auto maskBinStr = formatBinExpr(mask, {false, true, false});
    int countZeroes = 0;
    size_t offset = 0;
    for (; offset < maskBinStr.size(); ++offset) {
        if (maskBinStr.at(offset) == '0') {
            countZeroes++;
        } else {
            if (countZeroes > 0) {
                ignoreMasks.emplace_back(offset - countZeroes, countZeroes);
                countZeroes = 0;
            }
        }
    }
    if (countZeroes > 0) {
        ignoreMasks.emplace_back(offset - countZeroes, countZeroes);
    }
    return ignoreMasks;
}

inja::json PTF::getExpectedPacket(const TestSpec *testSpec) const {
    inja::json verifyData = inja::json::object();
    auto egressPacket = testSpec->getEgressPacket();
    if (egressPacket.has_value()) {
        const auto *packet = egressPacket.value();
        verifyData["eg_port"] = packet->getPort();
        const auto *payload = packet->getEvaluatedPayload();
        const auto *payloadMask = packet->getEvaluatedPayloadMask();
        verifyData["ignore_masks"] = getIgnoreMasks(payloadMask);

        auto dataStr = formatHexExpr(payload, {false, true, false});
        verifyData["exp_pkt"] = insertHexSeparators(dataStr);
    }
    return verifyData;
}

void PTF::emitPreamble() {
    static const std::string PREAMBLE(
        R"""(# P4Runtime PTF test for {{test_name}}
# symbex seed: {{ default(seed, "none") }}

from enum import Enum

from ptf.mask import Mask

from ptf.packet import *
from ptf import testutils as ptfutils


import base_test as bt


class AbstractTest(bt.P4RuntimeTest):
    EnumColor = Enum("EnumColor", ["GREEN", "YELLOW", "RED"], start=0)

    def setUp(self):
        bt.P4RuntimeTest.setUp(self)
        success = bt.P4RuntimeTest.updateConfig(self)
        assert success
        packet_wait_time = ptfutils.test_param_get("packet_wait_time")
        if not packet_wait_time:
            self.packet_wait_time = 0.1
        else:
            self.packet_wait_time = float(packet_wait_time)


    def tearDown(self):
        bt.P4RuntimeTest.tearDown(self)

    def setupCtrlPlane(self):
        pass

    def sendPacket(self):
        pass

    def verifyPackets(self):
        pass

    @bt.autocleanup
    def runTestImpl(self):
        self.setupCtrlPlane()
        bt.testutils.log.info("Sending Packet ...")
        self.sendPacket()
        bt.testutils.log.info("Verifying Packet ...")
        self.verifyPackets()

    def meter_write_with_predefined_config(self, meter_name, index, value, direct):
        """Since we can not blast the target with packets, we have to carefully craft an artificial scenario where the meter will return the color we want. We do this by setting the meter config in such a way that the meter is forced to assign the desired color. For example, for RED to the lowest threshold values, to force a RED assignment."""
        value = self.EnumColor(value)
        if value == self.EnumColor.GREEN:
            meter_config = bt.p4runtime_pb2.MeterConfig(
                cir=4294967295, cburst=4294967295, pir=4294967295, pburst=4294967295
            )
        elif value == self.EnumColor.YELLOW:
            meter_config = bt.p4runtime_pb2.MeterConfig(
                cir=1, cburst=1, pir=4294967295, pburst=4294967295
            )
        elif value == self.EnumColor.RED:
            meter_config = bt.p4runtime_pb2.MeterConfig(
                cir=1, cburst=1, pir=1, pburst=1
            )
        else:
            raise self.failureException(f"Unsupported meter value {value}")
        if direct:
            meter_obj = self.get_obj("direct_meters", meter_name)
            table_id = meter_obj.direct_table_id
            req, _ = self.make_table_read_request_by_id(table_id)
            table_entry = None
            for response in self.response_dump_helper(req):
                for entity in response.entities:
                    assert entity.WhichOneof("entity") == "table_entry"
                    table_entry = entity.table_entry
                    break
            if table_entry is None:
                raise self.failureException(
                    "No entry in the table that the meter is attached to."
                )
            return self.direct_meter_write(meter_config, table_id, table_entry)
        return self.meter_write(meter_name, index, meter_config)
)""");

    inja::json dataJson;
    dataJson["test_name"] = getTestBackendConfiguration().testBaseName;
    auto optSeed = getTestBackendConfiguration().seed;
    if (optSeed.has_value()) {
        dataJson["seed"] = optSeed.value();
    }

    inja::render_to(ptfFileStream, PREAMBLE, dataJson);
    ptfFileStream.flush();
}

std::string PTF::getTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
class Test{{test_id}}(AbstractTest):
    '''
    Date generated: {{timestamp}}
    Current node coverage: {{coverage}}
## if length(selected_branches) > 0
    Selected branches: {{selected_branches}}
## endif
    Trace:
## for trace_item in trace
    {{trace_item}}
##endfor
    '''

    def setupCtrlPlane(self):
        # Simple noop that is always called as filler.
        pass
## if control_plane
## for table in control_plane.tables
## for rule in table.rules
        self.table_add(
            ('{{table.table_name}}',
            [
## for r in rule.rules.single_exact_matches
                self.Exact('{{r.field_name}}', {{r.value}}),
## endfor
## for r in rule.rules.optional_matches
                self.Optional('{{r.field_name}}', {{r.value}}, {{r.use_exact}}),
## endfor
## for r in rule.rules.range_matches
                self.Range('{{r.field_name}}', {{r.lo}}, {{r.hi}}),
## endfor
## for r in rule.rules.ternary_matches
                self.Ternary('{{r.field_name}}', {{r.value}}, {{r.mask}}),
## endfor
## for r in rule.rules.lpm_matches
                self.Lpm('{{r.field_name}}', {{r.value}}, {{r.prefix_len}}),
## endfor
            ]),
            ('{{rule.action_name}}',
            [
## for act_param in rule.rules.act_args
                ('{{act_param.param}}', {{act_param.value}}),
## endfor
            ])
            , {% if rule.rules.needs_priority %}{{rule.priority}}{% else %}None{% endif %}
            {% if existsIn(table, "has_ap") %}, {"oneshot": True}{% endif %}
        )
## endfor
## endfor
## endif
## if exists("clone_specs")
## for clone_pkt in clone_specs.clone_pkts
        self.insert_pre_clone_session({{clone_pkt.session_id}}, [{{clone_pkt.clone_port}}])
## endfor
## endif
## for meter_value in meter_values
        self.meter_write_with_predefined_config("{{meter_value.name}}", {{meter_value.index}}, {{meter_value.value}}, {{meter_value.is_direct}})
## endfor


    def sendPacket(self):
## if send
        ig_port = {{send.ig_port}}
        pkt = b'{{send.pkt}}'
        ptfutils.send_packet(self, ig_port, pkt)
## else
        pass
## endif

    def verifyPackets(self):
## if verify
        eg_port = {{verify.eg_port}}
        exp_pkt = Mask(b'{{verify.exp_pkt}}')
## for ignore_mask in verify.ignore_masks
        exp_pkt.set_do_not_care({{ignore_mask.0}}, {{ignore_mask.1}})
## endfor
## if exists("clone_specs")
## for clone_pkt in clone_specs.clone_pkts
## if clone_pkt.cloned
        ptfutils.verify_packet(self, exp_pkt, {{clone_pkt.clone_port}})
##endif
##endfor
## if not clone_specs.has_clone
        ptfutils.verify_packet(self, exp_pkt, eg_port)
##endif
## else 
        ptfutils.verify_packet(self, exp_pkt, eg_port)
        bt.testutils.log.info("Verifying no other packets ...")
        ptfutils.verify_no_other_packets(self, self.device_id, timeout=self.packet_wait_time)
## endif
## else
        ptfutils.verify_no_other_packets(self, self.device_id, timeout=self.packet_wait_time)
## endif

    def runTest(self):
        self.runTestImpl()

)""");
    return TEST_CASE;
}

void PTF::emitTestcase(const TestSpec *testSpec, cstring selectedBranches, size_t testId,
                       const std::string &testCase, float currentCoverage) {
    inja::json dataJson;
    if (selectedBranches != nullptr) {
        dataJson["selected_branches"] = selectedBranches.c_str();
    }

    dataJson["test_id"] = testId;
    dataJson["trace"] = getTrace(testSpec);
    dataJson["control_plane"] = getControlPlane(testSpec);
    dataJson["send"] = getSend(testSpec);
    dataJson["verify"] = getExpectedPacket(testSpec);
    dataJson["timestamp"] = Utils::getTimeStamp();
    std::stringstream coverageStr;
    coverageStr << std::setprecision(2) << currentCoverage;
    dataJson["coverage"] = coverageStr.str();

    // Check whether this test has a clone configuration.
    // These are special because they require additional instrumentation and produce two output
    // packets.
    auto cloneSpecs = testSpec->getTestObjectCategory("clone_specs"_cs);
    if (!cloneSpecs.empty()) {
        dataJson["clone_specs"] = getClone(cloneSpecs);
    }
    auto meterValues = testSpec->getTestObjectCategory("meter_values"_cs);
    dataJson["meter_values"] = getMeter(meterValues);

    LOG5("PTF backend: emitting testcase:" << std::setw(4) << dataJson);

    if (!preambleEmitted) {
        BUG_CHECK(getTestBackendConfiguration().fileBasePath.has_value(), "Base path is not set.");
        auto ptfFile = getTestBackendConfiguration().fileBasePath.value();
        ptfFile.replace_extension(".py");
        ptfFileStream = std::ofstream(ptfFile);
        emitPreamble();
        preambleEmitted = true;
    }
    inja::render_to(ptfFileStream, testCase, dataJson);
    ptfFileStream.flush();
}

void PTF::writeTestToFile(const TestSpec *testSpec, cstring selectedBranches, size_t testId,
                          float currentCoverage, unsigned char* testCoverage, int mapSize) {
    std::string testCase = getTestCaseTemplate();
    emitTestcase(testSpec, selectedBranches, testId, testCase, currentCoverage);
}

std::string PTF::getTamperingTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
class TamperingTest{{test_id}}_{{kind_tag}}(AbstractTest):
    '''
    Date generated: {{timestamp}}
    Current node coverage: {{coverage}}
## if length(selected_branches) > 0
    Selected branches: {{selected_branches}}
## endif
    Tampering test: phase1=read original, phase2=write tampered, phase3=dynamic (same as phase1)
    Trace:
## for trace_item in trace
    {{trace_item}}
##endfor
    '''

    def setupCtrlPlane(self):
        # Simple noop that is always called as filler.
        pass
## if control_plane
## for table in control_plane.tables
## for rule in table.rules
        self.table_add(
            ('{{table.table_name}}',
            [
## for r in rule.rules.single_exact_matches
                self.Exact('{{r.field_name}}', {{r.value}}),
## endfor
## for r in rule.rules.optional_matches
                self.Optional('{{r.field_name}}', {{r.value}}, {{r.use_exact}}),
## endfor
## for r in rule.rules.range_matches
                self.Range('{{r.field_name}}', {{r.lo}}, {{r.hi}}),
## endfor
## for r in rule.rules.ternary_matches
                self.Ternary('{{r.field_name}}', {{r.value}}, {{r.mask}}),
## endfor
## for r in rule.rules.lpm_matches
                self.Lpm('{{r.field_name}}', {{r.value}}, {{r.prefix_len}}),
## endfor
            ]),
            ('{{rule.action_name}}',
            [
## for act_param in rule.rules.act_args
                ('{{act_param.param}}', {{act_param.value}}),
## endfor
            ])
            , {% if rule.rules.needs_priority %}{{rule.priority}}{% else %}None{% endif %}
            {% if existsIn(table, "has_ap") %}, {"oneshot": True}{% endif %}
        )
## endfor
## endfor
## endif

    @bt.autocleanup
    def runTestImpl(self):
        self.setupCtrlPlane()
        # Phase 1: read original register value
        bt.testutils.log.info("Tampering Phase 1: read original value ...")
        ig_port = {{phase1_send.ig_port}}
        pkt = b'{{phase1_send.pkt}}'
        ptfutils.send_packet(self, ig_port, pkt)
## if phase1_verify
        exp_pkt = Mask(b'{{phase1_verify.exp_pkt}}')
## for ignore_mask in phase1_verify.ignore_masks
        exp_pkt.set_do_not_care({{ignore_mask.0}}, {{ignore_mask.1}})
## endfor
        ptfutils.verify_packet(self, exp_pkt, {{phase1_verify.eg_port}})
## else
        ptfutils.verify_no_other_packets(self, self.device_id, timeout=self.packet_wait_time)
## endif
        # Phase 2: write tampered register value
        bt.testutils.log.info("Tampering Phase 2: write tampered value ...")
        ig_port = {{phase2_send.ig_port}}
        pkt = b'{{phase2_send.pkt}}'
        ptfutils.send_packet(self, ig_port, pkt)
## if phase2_verify
        exp_pkt = Mask(b'{{phase2_verify.exp_pkt}}')
## for ignore_mask in phase2_verify.ignore_masks
        exp_pkt.set_do_not_care({{ignore_mask.0}}, {{ignore_mask.1}})
## endfor
        ptfutils.verify_packet(self, exp_pkt, {{phase2_verify.eg_port}})
## else
        ptfutils.verify_no_other_packets(self, self.device_id, timeout=self.packet_wait_time)
## endif
        # Phase 3: read tampered register value
        bt.testutils.log.info("Tampering Phase 3: read tampered value ...")
        ig_port = {{phase3_send.ig_port}}
        pkt = b'{{phase3_send.pkt}}'
        ptfutils.send_packet(self, ig_port, pkt)
## if phase3_verify
        exp_pkt = Mask(b'{{phase3_verify.exp_pkt}}')
## for ignore_mask in phase3_verify.ignore_masks
        exp_pkt.set_do_not_care({{ignore_mask.0}}, {{ignore_mask.1}})
## endfor
        ptfutils.verify_packet(self, exp_pkt, {{phase3_verify.eg_port}})
## else
        ptfutils.verify_no_other_packets(self, self.device_id, timeout=self.packet_wait_time)
## endif

    def runTest(self):
        self.runTestImpl()

)""");
    return TEST_CASE;
}

void PTF::emitTamperingTestcase(const TamperingTestSpec *testSpec, cstring selectedBranches,
                                 size_t chainId, size_t subTestId, const std::string &testCase,
                                 float currentCoverage) {
    // Combined id for the in-file Python class suffix: unique across a chain's sub-tests.
    size_t testId = chainId * 1000 + subTestId;
    inja::json dataJson;
    if (selectedBranches != nullptr) {
        dataJson["selected_branches"] = selectedBranches.c_str();
    }

    dataJson["test_id"] = testId;
    // PTF appends every test to ONE .py file, so the class name has to carry the tamper kind too:
    // the kinds reuse the same (chainId, subTestId) numbering, and two classes of the same name in
    // one module are not an error — the later definition silently wins at import, losing a whole
    // kind's tests. Kept out of test_id so h2m class names do not renumber.
    dataJson["kind_tag"] = tamperKindTag(testSpec->kind).c_str();
    dataJson["trace"] = getTrace(testSpec->spec1);

    // Build a merged control-plane JSON that includes table entries from both Phase 1
    // (read-action entries) and Phase 2 (write-action entries).
    {
        auto cpJson = inja::json::object();
        std::map<cstring, std::vector<const TableRule *>> mergedRules;
        auto collectRules = [&](const TestSpec *spec) {
            for (const auto &[name, obj] : spec->getTestObjectCategory("tables"_cs)) {
                const auto *cfg = obj->checkedTo<TableConfig>();
                for (const auto &rule : *cfg->getRules()) {
                    mergedRules[name].push_back(&rule);
                }
            }
        };
        collectRules(testSpec->spec1);
        collectRules(testSpec->spec2);

        if (!mergedRules.empty()) {
            cpJson["tables"] = inja::json::array();
            for (const auto &[name, rules] : mergedRules) {
                inja::json tblJson;
                tblJson["table_name"] = name;
                tblJson["rules"] = inja::json::array();
                for (const auto *rule : rules) {
                    inja::json ruleJson;
                    ruleJson["action_name"] = rule->getActionCall()->getActionName().c_str();
                    ruleJson["rules"] = getControlPlaneForTable(
                        *rule->getMatches(), *rule->getActionCall()->getArgs());
                    ruleJson["priority"] = rule->getPriority();
                    tblJson["rules"].push_back(ruleJson);
                }
                cpJson["tables"].push_back(tblJson);
            }
        }
        dataJson["control_plane"] = cpJson;
    }
    dataJson["timestamp"] = Utils::getTimeStamp();
    std::stringstream coverageStr;
    coverageStr << std::setprecision(2) << currentCoverage;
    dataJson["coverage"] = coverageStr.str();

    dataJson["phase1_send"] = getSend(testSpec->spec1);
    dataJson["phase1_verify"] = getExpectedPacket(testSpec->spec1);
    dataJson["phase2_send"] = getSend(testSpec->spec2);
    dataJson["phase2_verify"] = getExpectedPacket(testSpec->spec2);
    // Phase 3 replays Phase 1's input; no expected output (dynamic deviation check).
    dataJson["phase3_send"] = getSend(testSpec->spec1);
    dataJson["phase3_verify"] = false;

    LOG5("PTF tampering backend: emitting testcase:" << std::setw(4) << dataJson);

    if (!preambleEmitted) {
        BUG_CHECK(getTestBackendConfiguration().fileBasePath.has_value(), "Base path is not set.");
        auto ptfFile = getTestBackendConfiguration().fileBasePath.value();
        ptfFile.replace_extension(".py");
        ptfFileStream = std::ofstream(ptfFile);
        emitPreamble();
        preambleEmitted = true;
    }
    inja::render_to(ptfFileStream, testCase, dataJson);
    ptfFileStream.flush();
}

void PTF::writeTestToFile(const TamperingTestSpec *testSpec, cstring selectedBranches,
                           size_t chainId, size_t subTestId, float currentCoverage) {
    std::string testCase = getTamperingTestCaseTemplate();
    emitTamperingTestcase(testSpec, selectedBranches, chainId, subTestId, testCase, currentCoverage);
}

}  // namespace P4::P4Tools::Symbex::Bmv2
