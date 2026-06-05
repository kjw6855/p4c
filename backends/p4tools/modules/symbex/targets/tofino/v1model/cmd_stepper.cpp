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

#include "backends/p4tools/modules/symbex/targets/tofino/v1model/cmd_stepper.h"

#include <cstddef>
#include <map>
#include <optional>

#include "backends/p4tools/common/lib/constants.h"
#include "backends/p4tools/common/lib/variables.h"
#include "backends/tofino/bf-p4c/ir/gress.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "lib/cstring.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/packet_vars.h"

namespace P4::P4Tools::Symbex::Tofino {

using namespace P4::literals;

TofinoV1ModelCmdStepper::TofinoV1ModelCmdStepper(ExecutionState &state, AbstractSolver &solver,
                                                 const ProgramInfo &programInfo)
    : CmdStepper(state, solver, programInfo) {}

const TofinoV1ModelProgramInfo &TofinoV1ModelCmdStepper::getProgramInfo() const {
    return *CmdStepper::getProgramInfo().checkedTo<TofinoV1ModelProgramInfo>();
}

void TofinoV1ModelCmdStepper::initializeTargetEnvironment(ExecutionState &nextState) const {
    const auto &programInfo = getProgramInfo();
    const auto &target = SymbexTarget::get();
    const auto &archSpec = programInfo.getArchSpec();
    const auto *pipes = programInfo.getPipes();

    // v1model initializes all metadata to zero. Retrieve the type and initialize all relevant
    // block parameters to zero to avoid unnecessary taint.
    for (const auto &pipe : *pipes) {
        size_t blockIdx = 0;
        for (const auto &blockTuple : pipe.pipes) {
            const auto *typeDecl = blockTuple.second;
            const auto *archMember = archSpec.getArchMember(blockIdx);
            nextState.initializeBlockParams(target, typeDecl, &archMember->blockParams);
            blockIdx++;
        }
    }

    const auto *nineBitType = IR::Type_Bits::get(9);
    const auto *oneBitType = IR::Type_Bits::get(1);
    nextState.set(programInfo.getTargetInputPortVar(),
                  ToolsVariables::getSymbolicVariable(nineBitType, "tofino_v1model_ingress_port"_cs));
    // v1model implicitly sets the output port to 0.
    nextState.set(programInfo.getTargetOutputPortVar(), IR::Constant::get(nineBitType, 0));
    // Initialize parser_error with no error.
    const auto *parserErrVar =
        new IR::Member(programInfo.getParserErrorType(),
                       new IR::PathExpression("*standard_metadata"), "parser_error");
    nextState.set(parserErrVar, IR::Constant::get(parserErrVar->type, 0));
    // Initialize checksum_error with no error.
    const auto *checksumErrVar =
        new IR::Member(oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
    nextState.set(checksumErrVar, IR::Constant::get(checksumErrVar->type, 0));
    // The packet size metadata is the symbex packet length variable divided by 8.
    const auto *pktSizeType = &PacketVars::PACKET_SIZE_VAR_TYPE;
    const auto *packetSizeVar =
        new IR::Member(pktSizeType, new IR::PathExpression("*standard_metadata"), "packet_length");
    const auto *packetSizeConst = new IR::Div(pktSizeType, ExecutionState::getInputPacketSizeVar(),
                                              IR::Constant::get(pktSizeType, 8));
    nextState.set(packetSizeVar, packetSizeConst);

    // The reused Tofino expr stepper models a frame check sequence in the parser/emit path via the
    // "fcsLeft" property. v1model has no FCS, so initialize it to 0 (a no-op for those paths).
    nextState.setProperty("fcsLeft"_cs, static_cast<int64_t>(0));
}

std::optional<const Constraint *> TofinoV1ModelCmdStepper::startParserImpl(
    const IR::P4Parser *parser, ExecutionState &nextState) const {
    const auto &programInfo = getProgramInfo();
    // Map the parser error to the standard_metadata parser_error field (parameter index 3).
    const auto &errVar =
        programInfo.getParserParamVar(parser, programInfo.getParserErrorType(), 3, "parser_error"_cs);
    nextState.setParserErrorLabel(errVar);
    return std::nullopt;
}

std::map<Continuation::Exception, Continuation> TofinoV1ModelCmdStepper::getExceptionHandlers(
    const IR::P4Parser *parser, Continuation::Body /*normalContinuation*/,
    const ExecutionState & /*nextState*/) const {
    std::map<Continuation::Exception, Continuation> result;
    const auto &programInfo = getProgramInfo();
    auto gress = programInfo.getGress(parser);

    const auto &errVar =
        programInfo.getParserParamVar(parser, programInfo.getParserErrorType(), 3, "parser_error"_cs);

    switch (gress) {
        case INGRESS:
            // TODO: Implement the full TNA/v1model parser-error drop conditions. Currently, the
            // ingress parser sets parser_error on PacketTooShort and continues.
            result.emplace(Continuation::Exception::Reject, Continuation::Body({}));
            result.emplace(
                Continuation::Exception::PacketTooShort,
                Continuation::Body({new IR::AssignmentStatement(
                    errVar,
                    IR::Constant::get(errVar->type, P4Constants::PARSER_ERROR_PACKET_TOO_SHORT))}));
            // NoMatch will transition to the next block.
            result.emplace(Continuation::Exception::NoMatch, Continuation::Body({}));
            break;

        // The egress parser never drops the packet.
        case EGRESS:
            result.emplace(Continuation::Exception::NoMatch, Continuation::Body({}));
            break;
        case GHOST:
        default:
            BUG("Unimplemented thread: %1%", gress);
    }

    return result;
}

}  // namespace P4::P4Tools::Symbex::Tofino
