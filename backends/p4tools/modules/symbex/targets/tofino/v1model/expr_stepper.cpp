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

#include "backends/p4tools/modules/symbex/targets/tofino/v1model/expr_stepper.h"

#include <cstddef>
#include <vector>

#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/trace_event_types.h"
#include "ir/ir.h"
#include "ir/irutils.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/symbex/core/extern_info.h"
#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/constants.h"

namespace P4::P4Tools::Symbex::Tofino {

using namespace P4::literals;

TofinoV1ModelExprStepper::TofinoV1ModelExprStepper(ExecutionState &state, AbstractSolver &solver,
                                                   const ProgramInfo &programInfo)
    : Tofino1ExprStepper(state, solver, programInfo) {}

// v1model "method" externs ported from the bmv2/v1model target. These are architecture (v1model)
// semantics that the Tofino expr stepper does not provide; they are self-contained (operate on the
// execution state and standard_metadata, no bmv2 program-info dependency).
const TofinoV1ModelExprStepper::ExternMethodImpls<TofinoV1ModelExprStepper>
    TofinoV1ModelExprStepper::V1MODEL_EXTERN_METHOD_IMPLS({
        /* ======================================================================================
         *  mark_to_drop
         *  Sets standard_metadata.egress_spec to the drop port. Processed at the deparser.
         * ====================================================================================== */
        {"*method.mark_to_drop"_cs,
         {"standard_metadata"_cs},
         [](const ExternInfo &externInfo, TofinoV1ModelExprStepper &stepper) {
             auto &nextState = stepper.state.clone();
             const auto *nineBitType = IR::Type_Bits::get(Bmv2::BMv2Constants::PORT_BIT_WIDTH);
             const auto *metadataLabel =
                 externInfo.externArguments.at(0)->expression->checkedTo<IR::InOutReference>();
             const auto *portVar = new IR::Member(nineBitType, metadataLabel->ref, "egress_spec");
             nextState.set(portVar, IR::Constant::get(nineBitType, Bmv2::BMv2Constants::DROP_PORT));
             nextState.add(*new TraceEvents::Generic("mark_to_drop executed."_cs));
             nextState.popBody();
             stepper.result->emplace_back(nextState);
         }},
        /* ======================================================================================
         * verify_checksum
         *  Verifies the checksum of the supplied data. On mismatch standard_metadata.checksum_error
         *  is set. Only supported in the VerifyChecksum control.
         * ====================================================================================== */
        {"*method.verify_checksum"_cs,
         {"condition"_cs, "data"_cs, "checksum"_cs, "algo"_cs},
         [](const ExternInfo &externInfo, TofinoV1ModelExprStepper &stepper) {
             bool argsAreTainted = false;
             for (size_t idx = 0; idx < externInfo.externArguments.size(); ++idx) {
                 const auto *arg = externInfo.externArguments.at(idx);
                 argsAreTainted = argsAreTainted || Taint::hasTaint(arg->expression);
             }

             const auto *verifyCond = externInfo.externArguments.at(0)->expression;
             const auto *data = externInfo.externArguments.at(1)->expression;
             const auto *checksumValue = externInfo.externArguments.at(2)->expression;
             const auto *checksumValueType = checksumValue->type;
             const auto *algo = externInfo.externArguments.at(3)->expression;
             const auto *oneBitType = IR::Type_Bits::get(1);

             if (const auto *boolVal = verifyCond->to<IR::BoolLiteral>()) {
                 if (!boolVal->value) {
                     auto &taintedState = stepper.state.clone();
                     taintedState.popBody();
                     stepper.result->emplace_back(taintedState);
                     return;
                 }
             }

             if (argsAreTainted) {
                 auto &taintedState = stepper.state.clone();
                 const auto *checksumErr = new IR::Member(
                     oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
                 taintedState.set(checksumErr, stepper.getProgramInfo().createTargetUninitialized(
                                                   checksumErr->type, true));
                 taintedState.popBody();
                 stepper.result->emplace_back(taintedState);
                 return;
             }

             auto *checksumArgs = new IR::Vector<IR::Argument>();
             checksumArgs->push_back(new IR::Argument(checksumValue));
             checksumArgs->push_back(new IR::Argument(algo));
             checksumArgs->push_back(new IR::Argument(data));

             // The condition is true and the checksum matches.
             {
                 auto &nextState = stepper.state.clone();
                 const auto *concolicVar =
                     new IR::ConcolicVariable(checksumValueType, "*method_checksum"_cs, checksumArgs,
                                              externInfo.originalCall.clone_id, 0);
                 auto *checksumMatchCond = new IR::Equ(concolicVar, checksumValue);
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LAnd(checksumMatchCond, verifyCond),
                                              stepper.state, nextState);
             }
             // The condition is true and the checksum does not match.
             {
                 auto &nextState = stepper.state.clone();
                 auto *concolicVar =
                     new IR::ConcolicVariable(checksumValueType, "*method_checksum"_cs, checksumArgs,
                                              externInfo.originalCall.clone_id, 0);
                 std::vector<Continuation::Command> replacements;
                 auto *checksumMatchCond = new IR::Neq(concolicVar, checksumValue);
                 const auto *checksumErr = new IR::Member(
                     oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
                 const auto *assign =
                     new IR::AssignmentStatement(checksumErr, IR::Constant::get(oneBitType, 1));
                 auto *errorCond = new IR::LAnd(verifyCond, checksumMatchCond);
                 replacements.emplace_back(assign);
                 nextState.replaceTopBody(&replacements);
                 stepper.result->emplace_back(errorCond, stepper.state, nextState);
             }
             // The condition is false.
             {
                 auto &nextState = stepper.state.clone();
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LNot(IR::Type::Boolean::get(), verifyCond),
                                              stepper.state, nextState);
             }
         }},
        /* ======================================================================================
         * update_checksum
         *  Computes the checksum of the supplied data and writes it to the checksum parameter.
         *  Only supported in the ComputeChecksum control.
         * ====================================================================================== */
        {"*method.update_checksum"_cs,
         {"condition"_cs, "data"_cs, "checksum"_cs, "algo"_cs},
         [](const ExternInfo &externInfo, TofinoV1ModelExprStepper &stepper) {
             bool argsAreTainted = false;
             for (size_t idx = 0; idx < externInfo.externArguments.size() - 2; ++idx) {
                 const auto *arg = externInfo.externArguments.at(idx);
                 argsAreTainted = argsAreTainted || Taint::hasTaint(arg->expression);
             }

             const auto &checksumVar =
                 externInfo.externArguments.at(2)->expression->checkedTo<IR::InOutReference>()->ref;
             const auto *updateCond = externInfo.externArguments.at(0)->expression;
             const auto *checksumVarType = checksumVar->type;
             const auto *data = externInfo.externArguments.at(1)->expression;
             const auto *algo = externInfo.externArguments.at(3)->expression;

             if (const auto *boolVal = updateCond->to<IR::BoolLiteral>()) {
                 if (!boolVal->value) {
                     auto &taintedState = stepper.state.clone();
                     taintedState.popBody();
                     stepper.result->emplace_back(taintedState);
                     return;
                 }
             }

             if (argsAreTainted) {
                 auto &taintedState = stepper.state.clone();
                 taintedState.set(checksumVar, stepper.getProgramInfo().createTargetUninitialized(
                                                   checksumVarType, true));
                 taintedState.popBody();
                 stepper.result->emplace_back(taintedState);
                 return;
             }

             // The condition is true.
             {
                 auto *checksumArgs = new IR::Vector<IR::Argument>();
                 checksumArgs->push_back(new IR::Argument(checksumVar));
                 checksumArgs->push_back(new IR::Argument(algo));
                 checksumArgs->push_back(new IR::Argument(data));

                 auto &nextState = stepper.state.clone();
                 const auto *concolicVar =
                     new IR::ConcolicVariable(checksumVarType, "*method_checksum"_cs, checksumArgs,
                                              externInfo.originalCall.clone_id, 0);
                 nextState.set(checksumVar, concolicVar);
                 nextState.popBody();
                 stepper.result->emplace_back(updateCond, stepper.state, nextState);
             }
             // The condition is false. No change here.
             {
                 auto &nextState = stepper.state.clone();
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LNot(IR::Type::Boolean::get(), updateCond),
                                              stepper.state, nextState);
             }
         }},
        /* ======================================================================================
         * update_checksum_with_payload
         *  Identical to update_checksum, but includes the packet payload in the calculation.
         * ====================================================================================== */
        {"*method.update_checksum_with_payload"_cs,
         {"condition"_cs, "data"_cs, "checksum"_cs, "algo"_cs},
         [](const ExternInfo &externInfo, TofinoV1ModelExprStepper &stepper) {
             bool argsAreTainted = false;
             for (size_t idx = 0; idx < externInfo.externArguments.size() - 2; ++idx) {
                 const auto *arg = externInfo.externArguments.at(idx);
                 argsAreTainted = argsAreTainted || Taint::hasTaint(arg->expression);
             }

             const auto &checksumVar =
                 externInfo.externArguments.at(2)->expression->checkedTo<IR::InOutReference>()->ref;
             const auto *updateCond = externInfo.externArguments.at(0)->expression;
             const auto *checksumVarType = checksumVar->type;
             const auto *data = externInfo.externArguments.at(1)->expression;
             const auto *algo = externInfo.externArguments.at(3)->expression;

             if (argsAreTainted) {
                 auto &taintedState = stepper.state.clone();
                 taintedState.set(checksumVar, stepper.getProgramInfo().createTargetUninitialized(
                                                   checksumVarType, true));
                 taintedState.popBody();
                 stepper.result->emplace_back(taintedState);
                 return;
             }

             // The condition is true.
             {
                 auto *checksumArgs = new IR::Vector<IR::Argument>();
                 checksumArgs->push_back(new IR::Argument(checksumVar));
                 checksumArgs->push_back(new IR::Argument(algo));
                 checksumArgs->push_back(new IR::Argument(data));

                 auto &nextState = stepper.state.clone();
                 const auto *concolicVar =
                     new IR::ConcolicVariable(checksumVarType, "*method_checksum_with_payload"_cs,
                                              checksumArgs, externInfo.originalCall.clone_id, 0);
                 nextState.set(checksumVar, concolicVar);
                 nextState.popBody();
                 stepper.result->emplace_back(updateCond, stepper.state, nextState);
             }
             // The condition is false. No change here.
             {
                 auto &nextState = stepper.state.clone();
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LNot(IR::Type::Boolean::get(), updateCond),
                                              stepper.state, nextState);
             }
         }},
        /* ======================================================================================
         * verify_checksum_with_payload
         *  Identical to verify_checksum, but includes the packet payload in the calculation.
         * ====================================================================================== */
        {"*method.verify_checksum_with_payload"_cs,
         {"condition"_cs, "data"_cs, "checksum"_cs, "algo"_cs},
         [](const ExternInfo &externInfo, TofinoV1ModelExprStepper &stepper) {
             bool argsAreTainted = false;
             for (size_t idx = 0; idx < externInfo.externArguments.size(); ++idx) {
                 const auto *arg = externInfo.externArguments.at(idx);
                 argsAreTainted = argsAreTainted || Taint::hasTaint(arg->expression);
             }

             const auto *verifyCond = externInfo.externArguments.at(0)->expression;
             const auto *data = externInfo.externArguments.at(1)->expression;
             const auto *checksumValue = externInfo.externArguments.at(2)->expression;
             const auto *checksumValueType = checksumValue->type;
             const auto *algo = externInfo.externArguments.at(3)->expression;
             const auto *oneBitType = IR::Type_Bits::get(1);

             if (argsAreTainted) {
                 auto &taintedState = stepper.state.clone();
                 const auto *checksumErr = new IR::Member(
                     oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
                 taintedState.set(checksumErr, stepper.getProgramInfo().createTargetUninitialized(
                                                   checksumErr->type, true));
                 taintedState.popBody();
                 stepper.result->emplace_back(taintedState);
                 return;
             }

             auto *checksumArgs = new IR::Vector<IR::Argument>();
             checksumArgs->push_back(new IR::Argument(checksumValue));
             checksumArgs->push_back(new IR::Argument(algo));
             checksumArgs->push_back(new IR::Argument(data));

             // The condition is true and the checksum matches.
             {
                 auto &nextState = stepper.state.clone();
                 const auto *concolicVar =
                     new IR::ConcolicVariable(checksumValueType, "*method_checksum_with_payload"_cs,
                                              checksumArgs, externInfo.originalCall.clone_id, 0);
                 auto *checksumMatchCond = new IR::Equ(concolicVar, checksumValue);
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LAnd(checksumMatchCond, verifyCond),
                                              stepper.state, nextState);
             }
             // The condition is true and the checksum does not match.
             {
                 auto &nextState = stepper.state.clone();
                 auto *concolicVar =
                     new IR::ConcolicVariable(checksumValueType, "*method_checksum_with_payload"_cs,
                                              checksumArgs, externInfo.originalCall.clone_id, 0);
                 std::vector<Continuation::Command> replacements;
                 auto *checksumMatchCond = new IR::Neq(concolicVar, checksumValue);
                 const auto *checksumErr = new IR::Member(
                     oneBitType, new IR::PathExpression("*standard_metadata"), "checksum_error");
                 const auto *assign =
                     new IR::AssignmentStatement(checksumErr, IR::Constant::get(oneBitType, 1));
                 auto *errorCond = new IR::LAnd(verifyCond, checksumMatchCond);
                 replacements.emplace_back(assign);
                 nextState.replaceTopBody(&replacements);
                 stepper.result->emplace_back(errorCond, stepper.state, nextState);
             }
             // The condition is false. No change here.
             {
                 auto &nextState = stepper.state.clone();
                 nextState.popBody();
                 stepper.result->emplace_back(new IR::LNot(IR::Type::Boolean::get(), verifyCond),
                                              stepper.state, nextState);
             }
         }},
    });

void TofinoV1ModelExprStepper::evalExternMethodCall(const ExternInfo &externInfo) {
    auto method = V1MODEL_EXTERN_METHOD_IMPLS.find(externInfo.externObjectRef, externInfo.methodName,
                                                   externInfo.externArguments);
    if (method.has_value()) {
        return method.value()(externInfo, *this);
    }
    // Fall through to the Tofino externs (RegisterAction.execute, etc.), then the core stepper.
    return Tofino1ExprStepper::evalExternMethodCall(externInfo);
}

}  // namespace P4::P4Tools::Symbex::Tofino
