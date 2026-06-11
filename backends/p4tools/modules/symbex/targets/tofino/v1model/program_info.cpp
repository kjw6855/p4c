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

#include "backends/p4tools/modules/symbex/targets/tofino/v1model/program_info.h"

#include <map>
#include <utility>
#include <vector>

#include "backends/p4tools/common/lib/arch_spec.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "lib/cstring.h"
#include "lib/exceptions.h"
#include "lib/map.h"

#include "backends/p4tools/modules/symbex/lib/concolic.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/concolic.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/constants.h"
#include "backends/p4tools/modules/symbex/targets/tofino/concolic.h"

namespace P4::P4Tools::Symbex::Tofino {

using namespace P4::literals;

const IR::Type_Bits TofinoV1ModelProgramInfo::PARSER_ERR_BITS = IR::Type_Bits(32, false);

TofinoV1ModelProgramInfo::TofinoV1ModelProgramInfo(const TofinoCompilerResult &compilerResult,
                                                   std::vector<PipeInfo> inputPipes,
                                                   std::map<int, gress_t> declIdToGress,
                                                   std::map<int, size_t> declIdToPipe)
    : TofinoSharedProgramInfo(compilerResult, std::move(inputPipes), std::move(declIdToGress),
                              std::move(declIdToPipe)) {
    // Register the Tofino hash concolics (used by Hash externs) and the bmv2/v1model checksum
    // concolics. The v1model checksum externs (update/verify_checksum) emit the "*method_checksum"
    // concolic variable resolved by Bmv2Concolic; both targets compile into the same symbex
    // library, so we reuse Bmv2Concolic directly rather than duplicating it.
    concolicMethodImpls.add(*SharedTofinoConcolic::getSharedTofinoConcolicMethodImpls());
    concolicMethodImpls.add(*Bmv2::Bmv2Concolic::getBmv2ConcolicMethodImpls());

    // Restrict egress port to be an allowed valid port.
    const auto *validPortsCond = getValidPortConstraint(getTargetOutputPortVar());
    pipelineSequence.emplace_back(Continuation::Guard(validPortsCond));
}

const ArchSpec &TofinoV1ModelProgramInfo::getArchSpec() const { return ARCH_SPEC; }

const IR::Type_Bits *TofinoV1ModelProgramInfo::getParserErrorType() const { return &PARSER_ERR_BITS; }

std::vector<std::vector<Continuation::Command>> TofinoV1ModelProgramInfo::ingressCmds() const {
    return pipelineCmds(INGRESS);
}

std::vector<std::vector<Continuation::Command>> TofinoV1ModelProgramInfo::egressCmds() const {
    return pipelineCmds(EGRESS);
}

std::optional<const IR::Expression *> TofinoV1ModelProgramInfo::getPipePortRangeConstraint(
    const IR::StateVariable & /*portVar*/, size_t /*pipeIdx*/) const {
    // V1Switch programs are single-pipe; there is no Tofino pipe-number partition of the port.
    return {};
}

const IR::Expression *TofinoV1ModelProgramInfo::getValidPortConstraint(
    const IR::StateVariable &portVar) const {
    const auto &options = SymbexOptions::get();
    const IR::Expression *validPortsCond = new IR::BoolLiteral(true);
    // If the vector of permitted port ranges is not empty, restrict the possible output port.
    if (!options.permittedPortRanges.empty()) {
        const IR::Expression *cond = new IR::BoolLiteral(false);
        for (const auto &portRange : options.permittedPortRanges) {
            const auto *loVarOut = IR::Constant::get(portVar->type, portRange.first);
            const auto *hiVarOut = IR::Constant::get(portVar->type, portRange.second);
            cond = new IR::LOr(
                cond, new IR::LAnd(new IR::Leq(loVarOut, portVar), new IR::Leq(portVar, hiVarOut)));
        }
        validPortsCond = new IR::LAnd(validPortsCond, cond);
    }
    return validPortsCond;
}

std::vector<std::vector<Continuation::Command>> TofinoV1ModelProgramInfo::pipelineCmds(
    gress_t gress) const {
    // Compute the in-order execution sequence of the gress-local top-level blocks, including the
    // nodes that handle transitions between them. Mirrors the TNA pipelineCmds, but each block's
    // commands follow the bmv2/v1model semantics (no intrinsic-metadata prepend, no
    // check_tofino_drop).
    std::vector<std::vector<Continuation::Command>> pipelineSequences;
    const auto &archSpec = getArchSpec();

    for (const auto &pipeInfo : pipes) {
        pipelineSequences.emplace_back();
        auto &pipelineSequence = pipelineSequences.back();
        size_t blockIdx = 0;

        // Keep track of the pipe name throughout execution.
        pipelineSequence.emplace_back(
            Continuation::PropertyUpdate("pipe_name"_cs, pipeInfo.pipeName));

        for (const auto &decl : Values(pipeInfo.pipes)) {
            auto subCmds = processDeclaration(decl, blockIdx);
            if (getGress(decl) == gress) {
                pipelineSequence.insert(pipelineSequence.end(), subCmds.begin(), subCmds.end());
            }
            ++blockIdx;
        }
        BUG_CHECK(archSpec.getArchVectorSize() == blockIdx,
                  "The V1Switch architecture requires %1% blocks (provided %2%).",
                  archSpec.getArchVectorSize(), blockIdx);
    }
    return pipelineSequences;
}

std::vector<Continuation::Command> TofinoV1ModelProgramInfo::processDeclaration(
    const IR::Type_Declaration *typeDecl, size_t blockIdx) const {
    // Collect parameters.
    const auto *applyBlock = typeDecl->to<IR::IApply>();
    if (applyBlock == nullptr) {
        SYMBEX_UNIMPLEMENTED("Constructed type %s of type %s not supported.", typeDecl,
                              typeDecl->node_type_name());
    }
    // Retrieve the current canonical block in the architecture spec using the block index.
    const auto *archMember = getArchSpec().getArchMember(blockIdx);

    std::vector<Continuation::Command> cmds;
    // Copy-in.
    const auto *copyInCall = new IR::MethodCallStatement(Utils::generateInternalMethodCall(
        "copy_in", {IR::StringLiteral::get(typeDecl->name)}, IR::Type_Void::get(),
        new IR::ParameterList(
            {new IR::Parameter("blockRef", IR::Direction::In, IR::Type_Unknown::get())})));
    cmds.emplace_back(copyInCall);
    // Insert the actual pipeline.
    cmds.emplace_back(typeDecl);
    // Copy-out.
    const auto *copyOutCall = new IR::MethodCallStatement(Utils::generateInternalMethodCall(
        "copy_out", {IR::StringLiteral::get(typeDecl->name)}, IR::Type_Void::get(),
        new IR::ParameterList(
            {new IR::Parameter("blockRef", IR::Direction::In, IR::Type_Unknown::get())})));
    cmds.emplace_back(copyOutCall);
    auto *dropStmt =
        new IR::MethodCallStatement(Utils::generateInternalMethodCall("drop_and_exit", {}));

    // Update metadata variables for egress processing once Ingress is done (e.g. the egress port).
    if ((archMember->blockName == "Ingress")) {
        auto *egressPortVar =
            new IR::Member(IR::Type_Bits::get(Bmv2::BMv2Constants::PORT_BIT_WIDTH),
                           new IR::PathExpression("*standard_metadata"), "egress_port");
        auto *portStmt = new IR::AssignmentStatement(egressPortVar, getTargetOutputPortVar());
        cmds.emplace_back(portStmt);

        // Port validity is handled by the Tofino port model (getValidPortConstraint applied as an
        // egress guard in the constructor, plus getPipePortRangeConstraint in StartIngress/Egress),
        // not by the bmv2-style per-block input/output port guards, which conflict with it.

        // Multicast is not modeled precisely. As a sound over-approximation for tampering
        // observability, if a multicast group is set we forward the packet to a single
        // representative port instead of dropping it (the p4csd validator installs the group so
        // the replayed packet egresses this port). Set both egress_spec (so dropIsActive() is
        // false) and egress_port (the device output) to the representative port.
        const IR::Expression *mcastGroupVar = new IR::Member(
            IR::Type_Bits::get(16), new IR::PathExpression("*standard_metadata"), "mcast_grp");
        mcastGroupVar = new IR::Neq(mcastGroupVar, IR::Constant::get(IR::Type_Bits::get(16), 0));
        auto *mcastSpecAssign = new IR::AssignmentStatement(
            getTargetOutputPortVar(), IR::Constant::get(getTargetOutputPortVar()->type,
                                                        Bmv2::BMv2Constants::MULTICAST_REP_PORT));
        auto *mcastPortAssign = new IR::AssignmentStatement(
            egressPortVar,
            IR::Constant::get(egressPortVar->type, Bmv2::BMv2Constants::MULTICAST_REP_PORT));
        auto *mcastBody = new IR::BlockStatement({mcastSpecAssign, mcastPortAssign});
        auto *mcastStmt = new IR::IfStatement(mcastGroupVar, mcastBody, nullptr);
        cmds.emplace_back(mcastStmt);
    }
    // After the deparser, append the remaining packet payload and apply the drop decision.
    // Unlike bmv2, we do not invoke the traffic manager (clone/recirculate/resubmit are not
    // modeled here; programs that need them will surface a clean unimplemented-extern message).
    if ((archMember->blockName == "Deparser")) {
        const auto *stmt = new IR::MethodCallStatement(
            Utils::generateInternalMethodCall("prepend_emit_buffer", {}));
        cmds.emplace_back(stmt);
        const auto *dropCheck = new IR::IfStatement(dropIsActive(), dropStmt, nullptr);
        cmds.emplace_back(dropCheck);
    }
    return cmds;
}

const IR::StateVariable &TofinoV1ModelProgramInfo::getTargetInputPortVar() const {
    return *new IR::StateVariable(new IR::Member(IR::Type_Bits::get(Bmv2::BMv2Constants::PORT_BIT_WIDTH),
                                                 new IR::PathExpression("*standard_metadata"),
                                                 "ingress_port"));
}

const IR::StateVariable &TofinoV1ModelProgramInfo::getTargetOutputPortVar() const {
    return *new IR::StateVariable(new IR::Member(IR::Type_Bits::get(Bmv2::BMv2Constants::PORT_BIT_WIDTH),
                                                 new IR::PathExpression("*standard_metadata"),
                                                 "egress_spec"));
}

std::vector<const IR::StateVariable *> TofinoV1ModelProgramInfo::getMulticastGroupVars() const {
    return {new IR::StateVariable(new IR::Member(
        IR::Type_Bits::get(16), new IR::PathExpression("*standard_metadata"), "mcast_grp"))};
}

const IR::Expression *TofinoV1ModelProgramInfo::dropIsActive() const {
    const auto &egressPortVar = getTargetOutputPortVar();
    return new IR::Equ(IR::Constant::get(egressPortVar->type, Bmv2::BMv2Constants::DROP_PORT),
                       egressPortVar);
}

const IR::Expression *TofinoV1ModelProgramInfo::getOutPortConstraint(
    const IR::StateVariable &portVar, int minPortNo, int maxPortNo, std::vector<int> allowPorts) {
    const IR::Operation_Binary *portConstraint = new IR::LOr(
        new IR::Equ(portVar, new IR::Constant(portVar->type, Bmv2::BMv2Constants::DROP_PORT)),
        new IR::LAnd(new IR::Geq(portVar, new IR::Constant(portVar->type, minPortNo)),
                     new IR::Lss(portVar, new IR::Constant(portVar->type, maxPortNo))));
    for (int allowPort : allowPorts) {
        portConstraint = new IR::LOr(
            new IR::Equ(portVar, new IR::Constant(portVar->type, allowPort)), portConstraint);
    }
    return portConstraint;
}

const IR::Expression *TofinoV1ModelProgramInfo::getInPortConstraint(
    const IR::StateVariable &portVar, int minPortNo, int maxPortNo, std::vector<int> allowPorts) {
    const IR::Operation_Binary *portConstraint = new IR::LAnd(
        new IR::Neq(portVar, new IR::Constant(portVar->type, Bmv2::BMv2Constants::DROP_PORT)),
        new IR::LAnd(new IR::Geq(portVar, new IR::Constant(portVar->type, minPortNo)),
                     new IR::Lss(portVar, new IR::Constant(portVar->type, maxPortNo))));
    for (int allowPort : allowPorts) {
        portConstraint = new IR::LOr(
            new IR::Equ(portVar, new IR::Constant(portVar->type, allowPort)), portConstraint);
    }
    return portConstraint;
}

const ArchSpec TofinoV1ModelProgramInfo::ARCH_SPEC = ArchSpec(
    "V1Switch"_cs, {// parser Parser<H, M>(packet_in b, out H parsedHdr, inout M meta,
                    //                     inout standard_metadata_t standard_metadata);
                    {"Parser"_cs, {nullptr, "*hdr"_cs, "*meta"_cs, "*standard_metadata"_cs}},
                    // control VerifyChecksum<H, M>(inout H hdr, inout M meta);
                    {"VerifyChecksum"_cs, {"*hdr"_cs, "*meta"_cs}},
                    // control Ingress<H, M>(inout H hdr, inout M meta,
                    //                       inout standard_metadata_t standard_metadata);
                    {"Ingress"_cs, {"*hdr"_cs, "*meta"_cs, "*standard_metadata"_cs}},
                    // control Egress<H, M>(inout H hdr, inout M meta,
                    //                      inout standard_metadata_t standard_metadata);
                    {"Egress"_cs, {"*hdr"_cs, "*meta"_cs, "*standard_metadata"_cs}},
                    // control ComputeChecksum<H, M>(inout H hdr, inout M meta);
                    {"ComputeChecksum"_cs, {"*hdr"_cs, "*meta"_cs}},
                    // control Deparser<H>(packet_out b, in H hdr);
                    {"Deparser"_cs, {nullptr, "*hdr"_cs}}});

}  // namespace P4::P4Tools::Symbex::Tofino
