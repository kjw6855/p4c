#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend/protobuf.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <inja/inja.hpp>

#include "backends/p4tools/common/control_plane/p4info_map.h"
#include "backends/p4tools/common/lib/format_int.h"
#include "backends/p4tools/common/lib/util.h"
#include "control-plane/p4RuntimeSerializer.h"
#include "ir/ir.h"
#include "lib/exceptions.h"
#include "lib/log.h"
#include "nlohmann/json.hpp"

#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/tamper_kind.h"
#include "backends/p4tools/modules/symbex/lib/test_object.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/constants.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_spec.h"

namespace P4::P4Tools::Symbex::Bmv2 {

Protobuf::Protobuf(const TestBackendConfiguration &testBackendConfiguration,
                   P4::P4RuntimeAPI p4RuntimeApi)
    : Bmv2TestFramework(testBackendConfiguration),
      p4RuntimeApi(p4RuntimeApi),
      p4InfoMaps(P4::ControlPlaneAPI::P4InfoMaps(*p4RuntimeApi.p4Info)) {}

inja::json Protobuf::getControlPlane(const TestSpec *testSpec) const {
    inja::json controlPlaneJson = inja::json::object();

    // Map of actionProfiles and actionSelectors for easy reference.
    std::map<cstring, cstring> apAsMap;

    auto tables = testSpec->getTestObjectCategory("tables"_cs);
    if (!tables.empty()) {
        controlPlaneJson["tables"] = inja::json::array();
    }
    for (auto const &testObject : tables) {
        inja::json tblJson;
        auto tableName = testObject.first;
        tblJson["table_name"] = tableName;
        const auto *const tblConfig = testObject.second->checkedTo<TableConfig>();
        const auto *table = tblConfig->getTable();
        auto p4RuntimeId = p4InfoMaps.lookUpP4RuntimeId(table->controlPlaneName());
        BUG_CHECK(p4RuntimeId, "Id not present for table %1%. Can not generate test.", table);
        tblJson["id"] = p4RuntimeId.value();
        const auto *tblRules = tblConfig->getRules();
        tblJson["rules"] = inja::json::array();
        for (const auto &tblRule : *tblRules) {
            inja::json rule;
            const auto *matches = tblRule.getMatches();
            const auto *actionCall = tblRule.getActionCall();
            const auto *actionDecl = actionCall->getAction();
            cstring actionName = actionDecl->controlPlaneName();
            const auto *actionArgs = actionCall->getArgs();
            rule["action_name"] = actionCall->getActionName().c_str();
            auto p4RuntimeId = p4InfoMaps.lookUpP4RuntimeId(actionName);
            BUG_CHECK(p4RuntimeId, "Id not present for action %1%. Can not generate test.",
                      actionDecl);
            rule["action_id"] = p4RuntimeId.value();

            auto j = getControlPlaneForTable(tableName, actionName, *matches, *actionArgs);
            rule["rules"] = std::move(j);
            rule["priority"] = tblRule.getPriority();
            tblJson["rules"].push_back(rule);
        }

        // Collect action profiles and selectors associated with the table.
        checkForTableActionProfile<Bmv2V1ModelActionProfile, Bmv2V1ModelActionSelector>(
            tblJson, apAsMap, tblConfig);

        // Check whether the default action is overridden for this table. Such a config carries ZERO
        // TableRules -- the action lives in a table property -- so without this it would be
        // invisible to the rule-driven entities loop in the template and the emitted test would
        // replay against p4c's compiled-in default instead of the one the model assumed.
        if (auto defaultOverride = getDefaultOverride(tableName, tblConfig); !defaultOverride.empty())
            tblJson["default_override"] = std::move(defaultOverride);

        controlPlaneJson["tables"].push_back(tblJson);
    }

    // Collect declarations of action profiles.
    collectActionProfileDeclarations<Bmv2V1ModelActionProfile>(testSpec, controlPlaneJson, apAsMap);

    return controlPlaneJson;
}

inja::json Protobuf::getControlPlaneForTable(cstring tableName, cstring actionName,
                                             const TableMatchMap &matches,
                                             const std::vector<ActionArg> &args) const {
    inja::json rulesJson;

    rulesJson["single_exact_matches"] = inja::json::array();
    rulesJson["multiple_exact_matches"] = inja::json::array();
    rulesJson["range_matches"] = inja::json::array();
    rulesJson["ternary_matches"] = inja::json::array();
    rulesJson["lpm_matches"] = inja::json::array();
    rulesJson["optional_matches"] = inja::json::array();
    rulesJson["act_args"] = inja::json::array();
    rulesJson["needs_priority"] = false;

    for (auto const &match : matches) {
        auto const fieldName = match.first;
        auto const &fieldMatch = match.second;

        inja::json j;
        j["field_name"] = fieldName;
        auto combinedFieldName = tableName + "_" + fieldName;
        auto p4RuntimeId = p4InfoMaps.lookUpP4RuntimeId(combinedFieldName);
        BUG_CHECK(p4RuntimeId.has_value(), "Id not present for key. Can not generate test.");
        j["id"] = p4RuntimeId.value();

        // Iterate over the match fields and segregate them.
        if (const auto *elem = fieldMatch->to<Exact>()) {
            j["value"] = formatHexExpressionWithSeparators(*elem->getEvaluatedValue());
            rulesJson["single_exact_matches"].push_back(j);
        } else if (const auto *elem = fieldMatch->to<Range>()) {
            j["lo"] = formatHexExpressionWithSeparators(*elem->getEvaluatedLow());
            j["hi"] = formatHexExpressionWithSeparators(*elem->getEvaluatedHigh());
            rulesJson["range_matches"].push_back(j);
            // If the rule has a range match we need to add the priority.
            rulesJson["needs_priority"] = true;
        } else if (const auto *elem = fieldMatch->to<Ternary>()) {
            // A ternary key field means the table needs a priority (even if this entry wildcards
            // the field).
            rulesJson["needs_priority"] = true;
            // A ternary field with an all-zero mask is "don't care". P4Runtime rejects a match
            // field with a 0 mask (INVALID_ARGUMENT: "omit match field instead of using 0 mask");
            // omit the field entirely instead. Pre-existing (cross-phase) table entries can carry
            // such don't-care ternary keys, unlike solver-synthesized entries.
            const auto *maskConst = elem->getEvaluatedMask()->to<IR::Constant>();
            if (maskConst != nullptr && maskConst->value == 0) {
                continue;
            }
            j["value"] = formatHexExpressionWithSeparators(*elem->getEvaluatedValue());
            j["mask"] = formatHexExpressionWithSeparators(*elem->getEvaluatedMask());
            rulesJson["ternary_matches"].push_back(j);
        } else if (const auto *elem = fieldMatch->to<LPM>()) {
            j["value"] = formatHexExpressionWithSeparators(*elem->getEvaluatedValue());
            j["prefix_len"] = elem->getEvaluatedPrefixLength()->value.str();
            rulesJson["lpm_matches"].push_back(j);
        } else if (const auto *elem = fieldMatch->to<Optional>()) {
            j["value"] = formatHexExpr(elem->getEvaluatedValue()).c_str();
            rulesJson["needs_priority"] = true;
            rulesJson["optional_matches"].push_back(j);
        } else {
            SYMBEX_UNIMPLEMENTED("Unsupported table key match type \"%1%\"",
                                  fieldMatch->getObjectName());
        }
    }

    for (const auto &actArg : args) {
        inja::json j;
        j["param"] = actArg.getActionParamName().c_str();
        j["value"] = formatHexExpressionWithSeparators(*actArg.getEvaluatedValue());
        auto combinedParamName = actionName + "_" + actArg.getActionParamName();
        auto p4RuntimeId = p4InfoMaps.lookUpP4RuntimeId(combinedParamName);
        BUG_CHECK(p4RuntimeId.has_value(), "Id not present for parameter. Can not generate test.");
        j["id"] = p4RuntimeId.value();
        rulesJson["act_args"].push_back(j);
    }

    return rulesJson;
}

inja::json Protobuf::getDefaultOverride(cstring tableName, const TableConfig *tblConfig) const {
    const auto *overrideObj = tblConfig->getProperty("overriden_default_action"_cs, false);
    if (overrideObj == nullptr) return {};
    const auto *actionCall = overrideObj->checkedTo<ActionCall>();
    const auto *actionDecl = actionCall->getAction();
    if (actionDecl == nullptr) return {};
    const cstring actionName = actionDecl->controlPlaneName();

    inja::json j;
    j["action_name"] = actionCall->getActionName().c_str();
    auto actionId = p4InfoMaps.lookUpP4RuntimeId(actionName);
    BUG_CHECK(actionId, "Id not present for action %1%. Can not generate test.", actionDecl);
    j["action_id"] = actionId.value();
    // Empty match map: a default entry matches nothing by definition. This still yields the
    // `act_args` array with the P4Runtime parameter ids the entity needs.
    j["rules"] = getControlPlaneForTable(tableName, actionName, {}, *actionCall->getArgs());
    return j;
}

inja::json Protobuf::getSend(const TestSpec *testSpec) const {
    const auto *iPacket = testSpec->getIngressPacket();
    const auto *payload = iPacket->getEvaluatedPayload();
    inja::json sendJson;
    sendJson["ig_port"] = iPacket->getPort();
    sendJson["pkt"] = formatHexExpressionWithSeparators(*payload);
    sendJson["pkt_size"] = payload->type->width_bits();
    return sendJson;
}

inja::json Protobuf::getExpectedPacket(const TestSpec *testSpec) const {
    inja::json verifyData = inja::json::object();
    auto egressPacket = testSpec->getEgressPacket();
    if (egressPacket.has_value()) {
        const auto *packet = egressPacket.value();
        verifyData["eg_port"] = packet->getPort();
        const auto *payload = packet->getEvaluatedPayload();
        const auto *mask = packet->getEvaluatedPayloadMask();
        verifyData["ignore_mask"] = formatHexExpressionWithSeparators(*mask);
        verifyData["exp_pkt"] = formatHexExpressionWithSeparators(*payload);
    }
    return verifyData;
}

std::string Protobuf::getTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
# proto-file: p4symbex.proto
# proto-message: TestCase
# A P4TestGen-generated test case for {{test_name}}.p4
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

## if control_plane
## for table in control_plane.tables
## for rule in table.rules
# Table {{table.table_name}}
entities {
  table_entry {
    table_id: {{table.id}}
    table_name: "{{table.table_name}}"
## if rule.rules.needs_priority
    priority: {{rule.priority}}
## endif
## for r in rule.rules.single_exact_matches
    # Match field {{r.field_name}}
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      exact {
        value: "{{r.value}}"
      }
    }
## endfor
## for r in rule.rules.optional_matches
  # Match field {{r.field_name}}
  match {
    field_id: {{r.id}}
    field_name: "{{r.field_name}}"
    optional {
      value: "{{r.value}}"
    }
  }
## endfor
## for r in rule.rules.range_matches
    # Match field {{r.field_name}}
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      range {
        low: "{{r.lo}}"
        high: "{{r.hi}}"
      }
    }
## endfor
## for r in rule.rules.ternary_matches
    # Match field {{r.field_name}}
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      ternary {
        value: "{{r.value}}"
        mask: "{{r.mask}}"
      }
    }
## endfor
## for r in rule.rules.lpm_matches
    # Match field {{r.field_name}}
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      lpm {
        value: "{{r.value}}"
        prefix_len: {{r.prefix_len}}
      }
    }
## endfor
    # Action {{rule.action_name}}
    action {
## if existsIn(table, "has_as")
      action_selector_name: "{{table.action_selector_name}}"
## endif
## if existsIn(table, "has_ap")
      action_profile_action_set {
        action_profile_actions {
          action {
            action_id: {{rule.action_id}}
            action_name: "{{rule.action_name}}"
## for act_param in rule.rules.act_args
            # Param {{act_param.param}}
            params {
              param_id: {{act_param.id}}
              param_name: "{{act_param.param}}"
              value: "{{act_param.value}}"
            }
## endfor
          }
        }
      }
## else
      action {
        action_id: {{rule.action_id}}
        action_name: "{{rule.action_name}}"
## for act_param in rule.rules.act_args
        # Param {{act_param.param}}
        params {
          param_id: {{act_param.id}}
          param_name: "{{act_param.param}}"
          value: "{{act_param.value}}"
        }
## endfor
      }
## endif
    }
  }
}
## endfor
## if existsIn(table, "default_override")
# Table {{table.table_name}} (default action)
entities {
  table_entry {
    table_id: {{table.id}}
    table_name: "{{table.table_name}}"
    is_default_action: true
    action {
      action {
        action_id: {{table.default_override.action_id}}
        action_name: "{{table.default_override.action_name}}"
## for act_param in table.default_override.rules.act_args
        # Param {{act_param.param}}
        params {
          param_id: {{act_param.id}}
          param_name: "{{act_param.param}}"
          value: "{{act_param.value}}"
        }
## endfor
      }
    }
  }
}
## endif
## endfor
## endif
)""");
    return TEST_CASE;
}

inja::json Protobuf::produceTestCase(const TestSpec *testSpec, cstring selectedBranches,
                                     size_t testId, float currentCoverage,
                                     unsigned char* testCoverage, int mapSize) const {
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
    dataJson["verify"] = getExpectedPacket(testSpec);
    dataJson["timestamp"] = Utils::getTimeStamp();
    std::stringstream coverageStr;
    coverageStr << std::setprecision(2) << currentCoverage;
    dataJson["coverage"] = coverageStr.str();

    dataJson["local_cov_size"] = mapSize;
    if (mapSize) {
        std::stringstream testCoverageMapStr;
        int allocLen = (mapSize / 8) + 1;
        for (int i = 0; i < allocLen; i++) {
            testCoverageMapStr << "\\x" << std::setw(2) << std::setfill('0')
                << std::hex << (unsigned int)testCoverage[i];
        }
        dataJson["local_coverage"] = testCoverageMapStr.str();
    } else {
        dataJson["local_coverage"] = "";
    }

    // Check whether this test has a clone configuration.
    // These are special because they require additional instrumentation and produce two output
    // packets.
    auto cloneSpecs = testSpec->getTestObjectCategory("clone_specs"_cs);
    if (!cloneSpecs.empty()) {
        dataJson["clone_specs"] = getClone(cloneSpecs);
    }
    auto meterValues = testSpec->getTestObjectCategory("meter_values"_cs);
    dataJson["meter_values"] = getMeter(meterValues);

    return dataJson;
}

void Protobuf::writeTestToFile(const TestSpec *testSpec, cstring selectedBranches, size_t testId,
                               float currentCoverage, unsigned char* testCoverage, int mapSize) {
    inja::json dataJson = produceTestCase(testSpec, selectedBranches, testId, currentCoverage, testCoverage, mapSize);
    LOG5("Protobuf test back end: emitting testcase:" << std::setw(4) << dataJson);

    auto optBasePath = getTestBackendConfiguration().fileBasePath;
    BUG_CHECK(optBasePath.has_value(), "Base path is not set.");
    auto incrementedbasePath = optBasePath.value();
    incrementedbasePath.concat("_" + std::to_string(testId));
    incrementedbasePath.replace_extension(".txtpb");
    auto protobufFileStream = std::ofstream(incrementedbasePath);
    inja::render_to(protobufFileStream, getTestCaseTemplate(), dataJson);
    protobufFileStream.flush();
}

AbstractTestReferenceOrError Protobuf::produceTest(const TestSpec *testSpec,
                                                   cstring selectedBranches, size_t testId,
                                                   float currentCoverage, unsigned char* testCoverage,
                                                   int mapSize) {
    inja::json dataJson = produceTestCase(testSpec, selectedBranches, testId, currentCoverage, testCoverage, mapSize);
    LOG5("ProtobufIR test back end: generated testcase:" << std::setw(4) << dataJson);

    return new ProtobufTest(inja::render(getTestCaseTemplate(), dataJson));
}

std::string Protobuf::getTamperingTestCaseTemplate() {
    static std::string TEST_CASE(
        R"""(
# proto-file: p4symbex.proto
# proto-message: TamperingTestCase
# A P4TestGen-generated tampering test case for {{test_name}}.p4
metadata: "symbex seed: {{ default(seed, "none") }}"
metadata: "Date generated: {{timestamp}}"
## if length(selected_branches) > 0
metadata: "{{selected_branches}}"
## endif
metadata: "Current node coverage: {{coverage}}"
metadata: "Tampering test: phase1=read original, phase2=write tampered, phase3=replay phase1 (dynamic deviation check)"
## if length(tamper_case) > 0
metadata: "Tamper case: {{tamper_case}}"
## endif

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

# Phase 2: write tampered register value
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

# Phase 3: replay Phase 1 input — dynamic deviation check (the validator compares the Phase-3
# output to Phase 1's reference to detect drop/port/byte divergence).
input_packet {
  packet: "{{phase3_send.pkt}}"
  port: {{phase3_send.ig_port}}
}

## for reg in affected_registers
affected_register {
  register_name: "{{reg.name}}"
  index: {{reg.index}}
  attacker_value: "{{reg.value}}"
## if existsIn(reg, "min_value")
  match_kind: REGISTER_MATCH_AT_LEAST
  min_value: "{{reg.min_value}}"
## endif
## if existsIn(reg, "sink_table")
  sink_table: "{{reg.sink_table}}"
## if existsIn(reg, "hit_phase")
  hit_phase: {{reg.hit_phase}}
  miss_phase: {{reg.miss_phase}}
## endif
## endif
## if existsIn(reg, "sink_outcome_legit")
  sink_outcome_legit: "{{reg.sink_outcome_legit}}"
  sink_outcome_attack: "{{reg.sink_outcome_attack}}"
## endif
}
## endfor

## if has_multicast
# Multicast forward modeled as a single representative port; the harness installs this
# group (mgid -> replica_port) before replay and removes it afterwards.
multicast_group {
  mgid: {{multicast_group.mgid}}
  replica_port: {{multicast_group.replica_port}}
}
## endif

## if control_plane
## for table in control_plane.tables
## for rule in table.rules
# Table {{table.table_name}} (Phase {{rule.phase}})
entities {
  table_entry {
    table_id: {{table.id}}
    table_name: "{{table.table_name}}"
## if rule.rules.needs_priority
    priority: {{rule.priority}}
## endif
## for r in rule.rules.single_exact_matches
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      exact {
        value: "{{r.value}}"
      }
    }
## endfor
## for r in rule.rules.optional_matches
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      optional {
        value: "{{r.value}}"
      }
    }
## endfor
## for r in rule.rules.range_matches
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      range {
        low: "{{r.lo}}"
        high: "{{r.hi}}"
      }
    }
## endfor
## for r in rule.rules.ternary_matches
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      ternary {
        value: "{{r.value}}"
        mask: "{{r.mask}}"
      }
    }
## endfor
## for r in rule.rules.lpm_matches
    match {
      field_id: {{r.id}}
      field_name: "{{r.field_name}}"
      lpm {
        value: "{{r.value}}"
        prefix_len: {{r.prefix_len}}
      }
    }
## endfor
    action {
## if existsIn(table, "has_ap")
      action_profile_action_set {
        action_profile_actions {
          action {
            action_id: {{rule.action_id}}
            action_name: "{{rule.action_name}}"
## for act_param in rule.rules.act_args
            params {
              param_id: {{act_param.id}}
              param_name: "{{act_param.param}}"
              value: "{{act_param.value}}"
            }
## endfor
          }
        }
      }
## else
      action {
        action_id: {{rule.action_id}}
        action_name: "{{rule.action_name}}"
## for act_param in rule.rules.act_args
        params {
          param_id: {{act_param.id}}
          param_name: "{{act_param.param}}"
          value: "{{act_param.value}}"
        }
## endfor
      }
## endif
    }
  }
}
## endfor
## if existsIn(table, "default_overrides")
## for ovr in table.default_overrides
# Table {{table.table_name}} (Phase {{ovr.phase}}, default action)
entities {
  table_entry {
    table_id: {{table.id}}
    table_name: "{{table.table_name}}"
    is_default_action: true
    action {
      action {
        action_id: {{ovr.action_id}}
        action_name: "{{ovr.action_name}}"
## for act_param in ovr.rules.act_args
        params {
          param_id: {{act_param.id}}
          param_name: "{{act_param.param}}"
          value: "{{act_param.value}}"
        }
## endfor
      }
    }
  }
}
## endfor
## endif
## endfor
## endif
)""");
    return TEST_CASE;
}

inja::json Protobuf::produceTamperingTestCase(const TamperingTestSpec *testSpec,
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

    // Build a merged control-plane JSON that includes table entries from both Phase 1
    // (read-action entries) and Phase 2 (write-action entries), preserving P4Runtime IDs.
    {
        inja::json controlPlaneJson = inja::json::object();
        // tableName → ordered vector of (TableConfig*, TableRule*) pairs to preserve rule order
        std::map<cstring, std::pair<const TableConfig *, std::vector<std::pair<int, const TableRule *>>>>
            mergedByTable;
        // tableName → ordered (phase, TableConfig*) for configs whose DEFAULT ACTION was overridden.
        // Kept separate from the rule map because such a config has zero rules; merging them would
        // mean synthesizing a match-less TableRule, which is indistinguishable from a genuinely
        // keyless match entry both here and to the replay driver.
        std::map<cstring, std::vector<std::pair<int, const TableConfig *>>> defaultsByTable;
        auto collectRules = [&](const TestSpec *spec, int phaseId) {
            for (const auto &[name, obj] : spec->getTestObjectCategory("tables"_cs)) {
                const auto *cfg = obj->checkedTo<TableConfig>();
                auto &entry = mergedByTable[name];
                if (entry.first == nullptr) entry.first = cfg;
                for (const auto &rule : *cfg->getRules()) {
                    entry.second.push_back({phaseId, &rule});
                }
                if (cfg->getProperty("overriden_default_action"_cs, false) != nullptr)
                    defaultsByTable[name].push_back({phaseId, cfg});
            }
        };
        collectRules(testSpec->spec1, 1);
        collectRules(testSpec->spec2, 2);

        if (!mergedByTable.empty()) {
            controlPlaneJson["tables"] = inja::json::array();
            for (const auto &[tableName, entry] : mergedByTable) {
                const auto *refCfg = entry.first;
                const auto &rules = entry.second;
                inja::json tblJson;
                tblJson["table_name"] = tableName;
                const auto *table = refCfg->getTable();
                auto tableId = p4InfoMaps.lookUpP4RuntimeId(table->controlPlaneName());
                BUG_CHECK(tableId, "Id not present for table %1%. Can not generate test.", table);
                tblJson["id"] = tableId.value();
                tblJson["rules"] = inja::json::array();
                for (const auto &[phaseId, tblRule] : rules) {
                    inja::json rule;
                    rule["phase"] = phaseId;
                    const auto *actionCall = tblRule->getActionCall();
                    const auto *actionDecl = actionCall->getAction();
                    cstring actionName = actionDecl->controlPlaneName();
                    rule["action_name"] = actionCall->getActionName().c_str();
                    auto actionId = p4InfoMaps.lookUpP4RuntimeId(actionName);
                    BUG_CHECK(actionId, "Id not present for action %1%. Can not generate test.",
                              actionDecl);
                    rule["action_id"] = actionId.value();
                    rule["rules"] = getControlPlaneForTable(tableName, actionName,
                                                            *tblRule->getMatches(),
                                                            *actionCall->getArgs());
                    rule["priority"] = tblRule->getPriority();
                    tblJson["rules"].push_back(rule);
                }
                // Emitted per phase, like the keyed rules above. The cross-phase control-plane check
                // already forces both phases onto ONE agreed default action, so these duplicate
                // rather than conflict, install dedup collapses them, and carrying both keeps the
                // harness lint's cross-phase action-data comparison alive for this table.
                if (auto defIt = defaultsByTable.find(tableName); defIt != defaultsByTable.end()) {
                    tblJson["default_overrides"] = inja::json::array();
                    for (const auto &[phaseId, cfg] : defIt->second) {
                        auto ovr = getDefaultOverride(tableName, cfg);
                        if (ovr.empty()) continue;
                        ovr["phase"] = phaseId;
                        tblJson["default_overrides"].push_back(std::move(ovr));
                    }
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
    dataJson["phase1_verify"] = getExpectedPacket(testSpec->spec1);
    dataJson["phase2_send"] = getSend(testSpec->spec2);
    dataJson["phase2_verify"] = getExpectedPacket(testSpec->spec2);
    // Phase 3 replays Phase 1's input packet; the validator detects the deviation dynamically.
    dataJson["phase3_send"] = getSend(testSpec->spec1);
    dataJson["tamper_case"] = testSpec->caseLabel.c_str();

    // Emit affected_register entries for each attacker-chosen write in Phase 2.
    inja::json affectedRegsJson = inja::json::array();
    for (const auto &[regName, regObj] : testSpec->attackerRegisterValues) {
        const auto *regVal = regObj->checkedTo<Bmv2V1ModelRegisterValue>();
        // sink_table metadata is populated only for Key-sink chains; absent for
        // chains whose sink is a header field or condition.
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
            j["value"] = formatHexExpressionWithSeparators(*valConst);
            // AT_LEAST criterion, when the analytical driver solved a flip value. Emitted only for
            // those cases, so an exact-match case is byte-identical to before.
            auto minIt = testSpec->attackerRegisterMinValues.find(regName);
            if (minIt != testSpec->attackerRegisterMinValues.end() && minIt->second > 0) {
                const auto *minConst = IR::Constant::get(valConst->type, minIt->second);
                j["min_value"] = formatHexExpressionWithSeparators(*minConst);
            }
            if (!sinkTableList.isNullOrEmpty()) {
                j["sink_table"] = sinkTableList.c_str();
                // Only the two HIT/MISS kinds have a hit/miss phase to name. An action divergence
                // reaches the sink in BOTH runs, so any pair of phase numbers here would be a
                // fiction — a fiction the harness would then lint against the installed keys.
                if (testSpec->kind == TamperKind::HitToMiss ||
                    testSpec->kind == TamperKind::MissToHit) {
                    // HIT→MISS: sink HITs in Phase 1, MISSes after tamper in Phase 3.
                    // MISS→HIT: sink MISSes in Phase 1, HITs after tamper in Phase 3.
                    j["hit_phase"] = testSpec->isMissToHit() ? 3 : 1;
                    j["miss_phase"] = testSpec->isMissToHit() ? 1 : 3;
                }
            }
            // Report-only outcome pair, outside the sink_table block so a condition sink (which has
            // no sink table) carries it too. The harness's txtpb readers look fields up by name, so
            // a key they do not know is ignored rather than mis-parsed.
            if (!testSpec->sinkOutcomeLegit.isNullOrEmpty()) {
                j["sink_outcome_legit"] = testSpec->sinkOutcomeLegit.c_str();
                j["sink_outcome_attack"] = testSpec->sinkOutcomeAttack.c_str();
            }
            affectedRegsJson.push_back(j);
        }
    }
    dataJson["affected_registers"] = affectedRegsJson;

    // Multicast forward (over-approximated as a single representative port): emit the group the
    // harness must install (mgid -> representative port) so the replayed packet egresses.
    dataJson["has_multicast"] = testSpec->usesMulticast;
    if (testSpec->usesMulticast) {
        inja::json mc;
        mc["mgid"] = testSpec->multicastGroupId;
        mc["replica_port"] = BMv2Constants::MULTICAST_REP_PORT;
        dataJson["multicast_group"] = mc;
    }

    return dataJson;
}

void Protobuf::writeTestToFile(const TamperingTestSpec *testSpec, cstring selectedBranches,
                                size_t chainId, size_t subTestId, float currentCoverage) {
    // Combined id for in-file metadata: stable and unique across a chain's sub-tests.
    size_t testId = chainId * 1000 + subTestId;
    inja::json dataJson = produceTamperingTestCase(testSpec, selectedBranches, testId, currentCoverage);
    LOG5("Protobuf tampering test back end: emitting testcase:" << std::setw(4) << dataJson);

    auto optBasePath = getTestBackendConfiguration().fileBasePath;
    BUG_CHECK(optBasePath.has_value(), "Base path is not set.");
    auto incrementedbasePath = optBasePath.value();
    // Include the tamper kind: the kinds are generated in separate passes but reuse the same
    // (chainId, subTestId) numbering, so without a kind tag the second pass silently overwrites the
    // first pass's files (losing the HIT→MISS tests, whose sink entry lives in Phase 1 and is
    // therefore replayable).
    const std::string dir = tamperKindTag(testSpec->kind).string();
    incrementedbasePath.concat("_" + std::to_string(chainId + 1) + "_" + std::to_string(subTestId) +
                               "_" + dir);
    incrementedbasePath.replace_extension(".txtpb");
    auto protobufFileStream = std::ofstream(incrementedbasePath);
    inja::render_to(protobufFileStream, getTamperingTestCaseTemplate(), dataJson);
    protobufFileStream.flush();
}

}  // namespace P4::P4Tools::Symbex::Bmv2
