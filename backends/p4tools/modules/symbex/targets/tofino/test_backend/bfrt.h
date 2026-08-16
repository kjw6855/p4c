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

#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TEST_BACKEND_BFRT_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TEST_BACKEND_BFRT_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <inja/inja.hpp>

#include "ir/ir.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"

namespace P4::P4Tools::Symbex::Tofino {

/// Extracts information from the @testSpec to emit a BfRt textproto test case.
///
/// The BFRT backend is the Tofino sibling of the BMv2 PROTOBUF backend. It
/// emits a single .txtpb file per generated test case, with a schema modelled
/// on `targets/tofino/proto/p4symbex_bfrt.proto`. The harness in
/// tools/p4csd/tofino/tofino_driver.py parses the .txtpb and replays the
/// entries against tofino_model via BF-Runtime gRPC.
///
/// Compared to PTF and STF, this backend is deliberately runtime-agnostic:
/// the .txtpb has no Python or test-runner-specific content, only canonical
/// BfRt entity descriptions plus the input/expected packet pairs.
class BfRt : public TestFramework {
 public:
    ~BfRt() override = default;

    BfRt(const BfRt &) = delete;
    BfRt(BfRt &&) = delete;
    BfRt &operator=(const BfRt &) = delete;
    BfRt &operator=(BfRt &&) = delete;

    explicit BfRt(const TestBackendConfiguration &testBackendConfiguration);

    /// Emit a single-phase test case as one .txtpb file.
    void writeTestToFile(const TestSpec *spec, cstring selectedBranches, size_t testIdx,
                         float currentCoverage, unsigned char *testCoverage, int mapSize) override;

    /// Emit a three-phase tampering test case as one .txtpb file with three
    /// input_packet / expected_output_packet blocks plus affected_register
    /// metadata and per-phase BfRt entities.
    void writeTestToFile(const TamperingTestSpec *spec, cstring selectedBranches, size_t chainId,
                         size_t subTestId, float currentCoverage) override;

 private:
    /// @returns the inja template for single-phase BfRt tests.
    static std::string getTestCaseTemplate();

    /// @returns the inja template for three-phase tampering BfRt tests.
    static std::string getTamperingTestCaseTemplate();

    /// Build the inja data JSON for a single-phase test.
    [[nodiscard]] inja::json produceTestCase(const TestSpec *testSpec, cstring selectedBranches,
                                             size_t testId, float currentCoverage,
                                             unsigned char *testCoverage, int mapSize) const;

    /// Build the inja data JSON for a three-phase tampering test.
    [[nodiscard]] inja::json produceTamperingTestCase(const TamperingTestSpec *testSpec,
                                                      cstring selectedBranches, size_t testId,
                                                      float currentCoverage) const;

    /// Convert all control plane objects (tables, register seeds, action profiles, ...)
    /// into BfRt-shaped inja JSON.
    static inja::json getControlPlane(const TestSpec *testSpec);

    /// Convert the ingress packet/port into inja JSON.
    static inja::json getSend(const TestSpec *testSpec);

    /// Convert the egress packet/port/mask into inja JSON.
    static inja::json getVerify(const TestSpec *testSpec);

    /// Helper for the control plane rule inja objects (key/data sets per match type).
    static inja::json getControlPlaneForTable(const TableMatchMap &matches,
                                              const std::vector<ActionArg> &args);

    /// The overridden default action of @p tblConfig, or a null json when the table has none.
    ///
    /// Such a config carries ZERO TableRules -- the action lives in the "overriden_default_action"
    /// table property -- so it is invisible to the rule-driven entities loop and would otherwise be
    /// dropped, leaving the replay to run p4c's compiled-in default instead of the one the model
    /// assumed. The arguments go through getControlPlaneForTable with an EMPTY match map so a
    /// default entry and a keyed entry can never disagree on how a value is rendered.
    static inja::json getDefaultOverride(const TableConfig *tblConfig);

    /// Helper for @getVerify: produce hex-escaped ignore mask spans for the egress mask.
    static std::vector<std::pair<size_t, size_t>> getIgnoreMasks(const IR::Constant *mask);
};

}  // namespace P4::P4Tools::Symbex::Tofino

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TEST_BACKEND_BFRT_H_ */
