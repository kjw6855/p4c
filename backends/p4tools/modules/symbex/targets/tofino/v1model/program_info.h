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

#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_PROGRAM_INFO_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_PROGRAM_INFO_H_

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "backends/tofino/bf-p4c/ir/gress.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/targets/tofino/shared_program_info.h"

namespace P4::P4Tools::Symbex::Tofino {

/// Program info for the (tofino, v1model) target: a v1model `V1Switch` program (the canonical
/// p4_14 -> p4_16 conversion output) whose stateful logic uses native Tofino `RegisterAction`
/// externs. The architecture/pipeline shape mirrors the bmv2/v1model target (6 V1Switch blocks
/// over standard_metadata), while extern semantics, the test backend, and the compiler config are
/// inherited from the Tofino device. See [[reference_p4symbex_tampering_architecture]].
class TofinoV1ModelProgramInfo : public TofinoSharedProgramInfo {
 private:
    /// The bit width of standard_metadata.parser_error in v1model (32 bits, unlike the 16-bit TNA
    /// parser error of the shared base).
    static const IR::Type_Bits PARSER_ERR_BITS;

    /// Imperative specification of the per-block execution (copy-in/out, port handoff, deparse).
    /// Ported from the bmv2/v1model target, minus the traffic-manager (clone/recirc) call.
    std::vector<Continuation::Command> processDeclaration(const IR::Type_Declaration *typeDecl,
                                                          size_t blockIdx) const;

    [[nodiscard]] std::vector<std::vector<Continuation::Command>> pipelineCmds(gress_t gress) const;

    [[nodiscard]] std::optional<const IR::Expression *> getPipePortRangeConstraint(
        const IR::StateVariable &portVar, size_t pipeIdx) const override;

    [[nodiscard]] const IR::Expression *getValidPortConstraint(
        const IR::StateVariable &portVar) const override;

 public:
    TofinoV1ModelProgramInfo(const TofinoCompilerResult &compilerResult,
                             std::vector<PipeInfo> inputPipes,
                             std::map<int, gress_t> declIdToGress,
                             std::map<int, size_t> declIdToPipe);

    /// @returns the constraint expression for a given port variable. Ported from bmv2/v1model.
    static const IR::Expression *getInPortConstraint(const IR::StateVariable &portVar, int minPortNo,
                                                     int maxPortNo, std::vector<int> allowPorts);

    static const IR::Expression *getOutPortConstraint(const IR::StateVariable &portVar,
                                                      int minPortNo, int maxPortNo,
                                                      std::vector<int> allowPorts);

    /// @see ProgramInfo::getArchSpec
    [[nodiscard]] const ArchSpec &getArchSpec() const override;

    [[nodiscard]] const IR::StateVariable &getTargetInputPortVar() const override;

    [[nodiscard]] const IR::StateVariable &getTargetOutputPortVar() const override;

    [[nodiscard]] std::vector<const IR::StateVariable *> getMulticastGroupVars() const override;

    [[nodiscard]] const IR::Expression *dropIsActive() const override;

    [[nodiscard]] const IR::Type_Bits *getParserErrorType() const override;

    [[nodiscard]] std::vector<std::vector<Continuation::Command>> ingressCmds() const override;

    [[nodiscard]] std::vector<std::vector<Continuation::Command>> egressCmds() const override;

    /// @see ProgramInfo::getArchSpec. The V1Switch 6-block layout (shared with bmv2/v1model).
    static const ArchSpec ARCH_SPEC;

    DECLARE_TYPEINFO(TofinoV1ModelProgramInfo, TofinoSharedProgramInfo);
};

}  // namespace P4::P4Tools::Symbex::Tofino

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_PROGRAM_INFO_H_ */
