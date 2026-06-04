/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include "backends/p4tools/modules/symbex/targets/tofino/test_backend/bfrt.h"

#include <ir/irutils.h>

#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <inja/inja.hpp>

#include "backends/p4tools/common/lib/format_int.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "lib/log.h"

#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/test_backend_configuration.h"
#include "backends/p4tools/modules/symbex/targets/tofino/test_spec.h"

namespace P4::P4Tools::Symbex::Tofino {

BfRt::BfRt(const TestBackendConfiguration &testBackendConfiguration)
    : TestFramework(testBackendConfiguration) {}

std::vector<std::pair<size_t, size_t>> BfRt::getIgnoreMasks(const IR::Constant *mask) {
    std::vector<std::pair<size_t, size_t>> ignoreMasks;
    if (mask == nullptr) {
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

inja::json BfRt::getControlPlaneForTable(const TableMatchMap &matches,
                                         const std::vector<ActionArg> &args) {
    inja::json rulesJson;

    rulesJson["exact_matches"] = inja::json::array();
    rulesJson["range_matches"] = inja::json::array();
    rulesJson["ternary_matches"] = inja::json::array();
    rulesJson["lpm_matches"] = inja::json::array();
    rulesJson["act_args"] = inja::json::array();
    rulesJson["needs_priority"] = false;

    for (auto const &match : matches) {
        auto const fieldName = match.first;
        auto const &fieldMatch = match.second;

        inja::json j;
        j["field_name"] = fieldName;
        if (const auto *elem = fieldMatch->to<Exact>()) {
            j["value"] = formatHexExpr(elem->getEvaluatedValue()).c_str();
            rulesJson["exact_matches"].push_back(j);
        } else if (const auto *elem = fieldMatch->to<Range>()) {
            j["lo"] = formatHexExpr(elem->getEvaluatedLow()).c_str();
            j["hi"] = formatHexExpr(elem->getEvaluatedHigh()).c_str();
            rulesJson["range_matches"].push_back(j);
            rulesJson["needs_priority"] = true;
        } else if (const auto *elem = fieldMatch->to<Ternary>()) {
            j["value"] = formatHexExpr(elem->getEvaluatedValue()).c_str();
            j["mask"] = formatHexExpr(elem->getEvaluatedMask()).c_str();
            rulesJson["ternary_matches"].push_back(j);
            rulesJson["needs_priority"] = true;
        } else if (const auto *elem = fieldMatch->to<LPM>()) {
            j["value"] = formatHexExpr(elem->getEvaluatedValue()).c_str();
            j["prefix_len"] = elem->getEvaluatedPrefixLength()->value.str();
            rulesJson["lpm_matches"].push_back(j);
        } else {
            SYMBEX_UNIMPLEMENTED("Unsupported table key match type \"%1%\"",
                                 fieldMatch->getObjectName());
        }
    }

    for (const auto &actArg : args) {
        inja::json j;
        j["param"] = actArg.getActionParamName().c_str();
        j["value"] = formatHexExpr(actArg.getEvaluatedValue()).c_str();
        rulesJson["act_args"].push_back(j);
    }

    return rulesJson;
}

inja::json BfRt::getControlPlane(const TestSpec *testSpec) {
    inja::json controlPlaneJson = inja::json::object();

    auto tables = testSpec->getTestObjectCategory("tables"_cs);
    if (!tables.empty()) {
        controlPlaneJson["tables"] = inja::json::array();
    }
    for (auto const &testObject : tables) {
        inja::json tblJson;
        tblJson["table_name"] = testObject.first.c_str();
        const auto *const tblConfig = testObject.second->checkedTo<TableConfig>();
        auto const *tblRules = tblConfig->getRules();
        tblJson["rules"] = inja::json::array();
        for (const auto &rule : *tblRules) {
            inja::json ruleJson;
            const auto *actionCall = rule.getActionCall();
            const auto *actionArgs = actionCall->getArgs();
            ruleJson["action_name"] = actionCall->getActionName().c_str();
            ruleJson["priority"] = rule.getPriority();
            ruleJson["rules"] = getControlPlaneForTable(*rule.getMatches(), *actionArgs);
            tblJson["rules"].push_back(ruleJson);
        }
        controlPlaneJson["tables"].push_back(tblJson);
    }

    // Initial register seeds (write the initial value at initialIndex before sending packet).
    auto registers = testSpec->getTestObjectCategory("registervalues"_cs);
    if (!registers.empty()) {
        controlPlaneJson["registers"] = inja::json::array();
    }
    for (auto const &[objName, obj] : registers) {
        const auto *reg = obj->checkedTo<TofinoRegisterValue>();
        inja::json r;
        r["name"] = reg->getRegisterDeclaration()->controlPlaneName();
        r["index"] = reg->getEvaluatedInitialIndex()->value.str();
        const auto *evaluatedVal = reg->getEvaluatedInitialValue();
        if (const auto *constantVal = evaluatedVal->to<IR::Constant>()) {
            r["init_val"] = formatHexExpr(constantVal);
        } else if (const auto *structExpr = evaluatedVal->to<IR::StructExpression>()) {
            r["init_val_list"] = inja::json::array();
            for (const auto *structElem : structExpr->components) {
                inja::json structElemJson = inja::json::object();
                structElemJson["name"] = structElem->name.name.c_str();
                structElemJson["val"] = formatHexExpr(structElem->expression);
                r["init_val_list"].push_back(structElemJson);
            }
        } else {
            P4C_UNIMPLEMENTED("Unsupported initial register value %1% of type %2%", evaluatedVal,
                              evaluatedVal->node_type_name());
        }
        controlPlaneJson["registers"].push_back(r);
    }

    return controlPlaneJson;
}

inja::json BfRt::getSend(const TestSpec *testSpec) {
    const auto *iPacket = testSpec->getIngressPacket();
    const auto *payload = iPacket->getEvaluatedPayload();
    inja::json sendJson;
    sendJson["ig_port"] = iPacket->getPort();
    auto dataStr = formatHexExpr(payload, {false, true, false});
    sendJson["pkt"] = insertHexSeparators(dataStr);
    sendJson["pkt_size"] = payload->type->width_bits();
    return sendJson;
}

inja::json BfRt::getVerify(const TestSpec *testSpec) {
    inja::json verifyData = inja::json::object();
    if (testSpec->getEgressPacket() == std::nullopt) {
        return inja::json(false);
    }
    const auto &packet = **testSpec->getEgressPacket();
    verifyData["eg_port"] = packet.getPort();
    const auto *payload = packet.getEvaluatedPayload();
    const auto *payloadMask = packet.getEvaluatedPayloadMask();
    auto dataStr = formatHexExpr(payload, {false, true, false});
    verifyData["exp_pkt"] = insertHexSeparators(dataStr);
    if (payloadMask != nullptr) {
        auto maskStr = formatHexExpr(payloadMask, {false, true, false});
        verifyData["ignore_mask"] = insertHexSeparators(maskStr);
    } else {
        // All bits significant — emit an all-FF mask of the same length as the payload.
        std::stringstream allFf;
        size_t numBytes = (payload->type->width_bits() + 7) / 8;
        for (size_t i = 0; i < numBytes; ++i) {
            allFf << "\\xff";
        }
        verifyData["ignore_mask"] = allFf.str();
    }
    return verifyData;
}

std::string BfRt::getTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
# proto-file: p4symbex_bfrt.proto
# proto-message: TestCase
# A P4Symbex-generated BfRt test case for {{test_name}}.p4
metadata: "symbex seed: {{ default(seed, "none") }}"
metadata: "Date generated: {{timestamp}}"
## if length(selected_branches) > 0
metadata: "{{selected_branches}}"
## endif
metadata: "Current node coverage: {{coverage}}"
stmt_cov_bitmap: "{{local_coverage}}"
stmt_cov_size: {{local_cov_size}}

## for trace_item in trace
traces: '{{trace_item}}'
## endfor

input_packet {
  packet: "{{send.pkt}}"
  port: {{send.ig_port}}
}

## if verify
expected_output_packet {
  packet: "{{verify.exp_pkt}}"
  port: {{verify.eg_port}}
  packet_mask: "{{verify.ignore_mask}}"
}
## endif

## if existsIn(control_plane, "tables")
## for table in control_plane.tables
## for rule in table.rules
# Table {{table.table_name}}
entities {
  table_name: "{{table.table_name}}"
  action_name: "{{rule.action_name}}"
## if rule.rules.needs_priority
  priority: {{rule.priority}}
## endif
## for r in rule.rules.exact_matches
  key {
    field_name: "{{r.field_name}}"
    exact { value: "{{r.value}}" }
  }
## endfor
## for r in rule.rules.range_matches
  key {
    field_name: "{{r.field_name}}"
    range { low: "{{r.lo}}" high: "{{r.hi}}" }
  }
## endfor
## for r in rule.rules.ternary_matches
  key {
    field_name: "{{r.field_name}}"
    ternary { value: "{{r.value}}" mask: "{{r.mask}}" }
  }
## endfor
## for r in rule.rules.lpm_matches
  key {
    field_name: "{{r.field_name}}"
    lpm { value: "{{r.value}}" prefix_len: {{r.prefix_len}} }
  }
## endfor
## for act_param in rule.rules.act_args
  data {
    field_name: "{{act_param.param}}"
    value: "{{act_param.value}}"
  }
## endfor
}
## endfor
## endfor
## endif
## if existsIn(control_plane, "registers")
## for reg in control_plane.registers
# Initial register seed for {{reg.name}}
register_seed {
  register_name: "{{reg.name}}"
  index: {{reg.index}}
## if existsIn(reg, "init_val")
  value: "{{reg.init_val}}"
## endif
## if existsIn(reg, "init_val_list")
## for elem in reg.init_val_list
  value_field {
    field_name: "{{elem.name}}"
    value: "{{elem.val}}"
  }
## endfor
## endif
}
## endfor
## endif
)""");
    return TEST_CASE;
}

inja::json BfRt::produceTestCase(const TestSpec *testSpec, cstring selectedBranches, size_t testId,
                                 float currentCoverage, unsigned char *testCoverage,
                                 int mapSize) const {
    inja::json dataJson;
    if (selectedBranches != nullptr) {
        dataJson["selected_branches"] = selectedBranches.c_str();
    }
    auto optSeed = getTestBackendConfiguration().seed;
    if (optSeed.has_value()) {
        dataJson["seed"] = optSeed.value();
    }
    dataJson["test_name"] = getTestBackendConfiguration().testBaseName;
    dataJson["test_id"] = testId;
    dataJson["trace"] = getTrace(testSpec);
    dataJson["control_plane"] = getControlPlane(testSpec);
    dataJson["send"] = getSend(testSpec);
    dataJson["verify"] = getVerify(testSpec);
    dataJson["timestamp"] = Utils::getTimeStamp();
    std::stringstream coverageStr;
    coverageStr << std::setprecision(2) << currentCoverage;
    dataJson["coverage"] = coverageStr.str();

    dataJson["local_cov_size"] = mapSize;
    if (mapSize) {
        std::stringstream testCoverageMapStr;
        int allocLen = (mapSize / 8) + 1;
        for (int i = 0; i < allocLen; i++) {
            testCoverageMapStr << "\\x" << std::setw(2) << std::setfill('0') << std::hex
                               << static_cast<unsigned int>(testCoverage[i]);
        }
        dataJson["local_coverage"] = testCoverageMapStr.str();
    } else {
        dataJson["local_coverage"] = "";
    }
    return dataJson;
}

void BfRt::writeTestToFile(const TestSpec *testSpec, cstring selectedBranches, size_t testId,
                           float currentCoverage, unsigned char *testCoverage, int mapSize) {
    inja::json dataJson =
        produceTestCase(testSpec, selectedBranches, testId, currentCoverage, testCoverage, mapSize);
    LOG5("BfRt backend: emitting testcase:" << std::setw(4) << dataJson);

    auto optBasePath = getTestBackendConfiguration().fileBasePath;
    BUG_CHECK(optBasePath.has_value(), "Base path is not set.");
    auto incrementedBasePath = optBasePath.value();
    incrementedBasePath.concat("_" + std::to_string(testId));
    incrementedBasePath.replace_extension(".txtpb");
    auto fileStream = std::ofstream(incrementedBasePath);
    inja::render_to(fileStream, getTestCaseTemplate(), dataJson);
    fileStream.flush();
}

std::string BfRt::getTamperingTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
# proto-file: p4symbex_bfrt.proto
# proto-message: TamperingTestCase
# A P4Symbex-generated three-phase tampering test case for {{test_name}}.p4
metadata: "symbex seed: {{ default(seed, "none") }}"
metadata: "Date generated: {{timestamp}}"
## if length(selected_branches) > 0
metadata: "{{selected_branches}}"
## endif
metadata: "Current node coverage: {{coverage}}"
metadata: "Tampering test: phase1=read original, phase2=write tampered, phase3=replay phase1 (dynamic deviation check)"

# --- Phase 1 (read) symbex trace ---
## for trace_item in trace
traces: '[P1] {{trace_item}}'
## endfor
# --- Phase 2 (write) symbex trace ---
## for trace_item in trace2
traces: '[P2] {{trace_item}}'
## endfor

# Phase 1: read original register value
input_packet {
  packet: "{{phase1_send.pkt}}"
  port: {{phase1_send.ig_port}}
}
## if phase1_verify
expected_output_packet {
  packet: "{{phase1_verify.exp_pkt}}"
  port: {{phase1_verify.eg_port}}
  packet_mask: "{{phase1_verify.ignore_mask}}"
}
## endif

# Phase 2: attacker writes tampered register value
input_packet {
  packet: "{{phase2_send.pkt}}"
  port: {{phase2_send.ig_port}}
}
## if phase2_verify
expected_output_packet {
  packet: "{{phase2_verify.exp_pkt}}"
  port: {{phase2_verify.eg_port}}
  packet_mask: "{{phase2_verify.ignore_mask}}"
}
## endif

# Phase 3: replay Phase 1 input — dynamic deviation check only
input_packet {
  packet: "{{phase3_send.pkt}}"
  port: {{phase3_send.ig_port}}
}

## for reg in affected_registers
affected_register {
  register_name: "{{reg.name}}"
  index: {{reg.index}}
  attacker_value: "{{reg.value}}"
## if existsIn(reg, "sink_table")
  sink_table: "{{reg.sink_table}}"
  hit_phase: 1
  miss_phase: 3
## endif
}
## endfor

## if existsIn(control_plane, "tables")
## for table in control_plane.tables
## for rule in table.rules
# Table {{table.table_name}} (Phase {{rule.phase}})
entities {
  table_name: "{{table.table_name}}"
  action_name: "{{rule.action_name}}"
## if rule.rules.needs_priority
  priority: {{rule.priority}}
## endif
  phase: {{rule.phase}}
## for r in rule.rules.exact_matches
  key {
    field_name: "{{r.field_name}}"
    exact { value: "{{r.value}}" }
  }
## endfor
## for r in rule.rules.range_matches
  key {
    field_name: "{{r.field_name}}"
    range { low: "{{r.lo}}" high: "{{r.hi}}" }
  }
## endfor
## for r in rule.rules.ternary_matches
  key {
    field_name: "{{r.field_name}}"
    ternary { value: "{{r.value}}" mask: "{{r.mask}}" }
  }
## endfor
## for r in rule.rules.lpm_matches
  key {
    field_name: "{{r.field_name}}"
    lpm { value: "{{r.value}}" prefix_len: {{r.prefix_len}} }
  }
## endfor
## for act_param in rule.rules.act_args
  data {
    field_name: "{{act_param.param}}"
    value: "{{act_param.value}}"
  }
## endfor
}
## endfor
## endfor
## endif
)""");
    return TEST_CASE;
}

inja::json BfRt::produceTamperingTestCase(const TamperingTestSpec *testSpec,
                                          cstring selectedBranches, size_t testId,
                                          float currentCoverage) const {
    inja::json dataJson;
    if (selectedBranches != nullptr) {
        dataJson["selected_branches"] = selectedBranches.c_str();
    }
    auto optSeed = getTestBackendConfiguration().seed;
    if (optSeed.has_value()) {
        dataJson["seed"] = optSeed.value();
    }
    dataJson["test_name"] = getTestBackendConfiguration().testBaseName;
    dataJson["test_id"] = testId;
    dataJson["trace"] = getTrace(testSpec->spec1);
    dataJson["trace2"] = getTrace(testSpec->spec2);

    // Build merged control-plane JSON spanning Phase 1 (read) and Phase 2 (write).
    {
        inja::json controlPlaneJson = inja::json::object();
        std::map<cstring,
                 std::pair<const TableConfig *, std::vector<std::pair<int, const TableRule *>>>>
            mergedByTable;
        auto collectRules = [&](const TestSpec *spec, int phaseId) {
            for (const auto &[name, obj] : spec->getTestObjectCategory("tables"_cs)) {
                const auto *cfg = obj->checkedTo<TableConfig>();
                auto &entry = mergedByTable[name];
                if (entry.first == nullptr) entry.first = cfg;
                for (const auto &rule : *cfg->getRules()) {
                    entry.second.push_back({phaseId, &rule});
                }
            }
        };
        collectRules(testSpec->spec1, 1);
        collectRules(testSpec->spec2, 2);

        if (!mergedByTable.empty()) {
            controlPlaneJson["tables"] = inja::json::array();
            for (const auto &[tableName, entry] : mergedByTable) {
                const auto &rules = entry.second;
                inja::json tblJson;
                tblJson["table_name"] = tableName.c_str();
                tblJson["rules"] = inja::json::array();
                for (const auto &[phaseId, tblRule] : rules) {
                    inja::json rule;
                    rule["phase"] = phaseId;
                    const auto *actionCall = tblRule->getActionCall();
                    rule["action_name"] = actionCall->getActionName().c_str();
                    rule["rules"] = getControlPlaneForTable(*tblRule->getMatches(),
                                                            *actionCall->getArgs());
                    rule["priority"] = tblRule->getPriority();
                    tblJson["rules"].push_back(rule);
                }
                controlPlaneJson["tables"].push_back(tblJson);
            }
        }
        dataJson["control_plane"] = controlPlaneJson;
    }

    dataJson["timestamp"] = Utils::getTimeStamp();
    std::stringstream coverageStr;
    coverageStr << std::setprecision(2) << currentCoverage;
    dataJson["coverage"] = coverageStr.str();

    dataJson["phase1_send"] = getSend(testSpec->spec1);
    dataJson["phase1_verify"] = getVerify(testSpec->spec1);
    dataJson["phase2_send"] = getSend(testSpec->spec2);
    dataJson["phase2_verify"] = getVerify(testSpec->spec2);
    // Phase 3 replays Phase 1's input; expected output is determined dynamically.
    dataJson["phase3_send"] = getSend(testSpec->spec1);

    // Emit affected_register entries for each attacker-chosen write in Phase 2.
    inja::json affectedRegsJson = inja::json::array();
    for (const auto &[regName, regObj] : testSpec->attackerRegisterValues) {
        const auto *regVal = regObj->checkedTo<TofinoRegisterValue>();
        // Look up the sink-table list once per register. Empty cstring (or
        // register absent) means this isn't a Key-sink chain — skip the
        // sink_table/hit_phase/miss_phase metadata in that case so the
        // harness falls back to the basic deviation check.
        cstring sinkTableList = ""_cs;
        auto sinkIt = testSpec->attackerRegisterSinkTables.find(regName);
        if (sinkIt != testSpec->attackerRegisterSinkTables.end()) {
            sinkTableList = sinkIt->second;
        }
        for (const auto &cond : regVal->getIndexConditions()) {
            const auto *idxConst = cond.getIndex()->checkedTo<IR::Constant>();
            const auto *valConst = cond.getValue()->checkedTo<IR::Constant>();
            inja::json j;
            j["name"] = regName.c_str();
            j["index"] = static_cast<int64_t>(static_cast<long long>(idxConst->value));
            j["value"] = insertHexSeparators(formatHexExpr(valConst, {false, true, false}));
            if (!sinkTableList.isNullOrEmpty()) {
                j["sink_table"] = sinkTableList.c_str();
            }
            affectedRegsJson.push_back(j);
        }
    }
    dataJson["affected_registers"] = affectedRegsJson;

    return dataJson;
}

void BfRt::writeTestToFile(const TamperingTestSpec *testSpec, cstring selectedBranches,
                           size_t chainId, size_t subTestId, float currentCoverage) {
    // Combined id for in-file metadata: stable and unique across a chain's sub-tests.
    size_t testId = chainId * 1000 + subTestId;
    inja::json dataJson =
        produceTamperingTestCase(testSpec, selectedBranches, testId, currentCoverage);
    LOG5("BfRt tampering backend: emitting testcase:" << std::setw(4) << dataJson);

    auto optBasePath = getTestBackendConfiguration().fileBasePath;
    BUG_CHECK(optBasePath.has_value(), "Base path is not set.");
    auto incrementedBasePath = optBasePath.value();
    incrementedBasePath.concat("_" + std::to_string(chainId + 1) + "_" + std::to_string(subTestId));
    incrementedBasePath.replace_extension(".txtpb");
    auto fileStream = std::ofstream(incrementedBasePath);
    inja::render_to(fileStream, getTamperingTestCaseTemplate(), dataJson);
    fileStream.flush();
}

}  // namespace P4::P4Tools::Symbex::Tofino
