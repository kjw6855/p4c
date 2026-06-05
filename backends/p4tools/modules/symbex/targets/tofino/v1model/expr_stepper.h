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

#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_EXPR_STEPPER_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_EXPR_STEPPER_H_

#include <string>

#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino/expr_stepper.h"

namespace P4::P4Tools::Symbex::Tofino {

/// Expression stepper for the (tofino, v1model) target. Layers the v1model "method" externs
/// (mark_to_drop, update/verify_checksum, random) on top of the Tofino1 expr stepper, which already
/// provides RegisterAction.execute and the rest of the Tofino extern surface. Externs not handled
/// here fall through to Tofino1ExprStepper, then to the core stepper (clean SYMBEX_UNIMPLEMENTED
/// for unmodeled v1model primitives such as clone/recirculate/resubmit/digest).
class TofinoV1ModelExprStepper : public Tofino1ExprStepper {
 protected:
    std::string getClassName() override { return "TofinoV1ModelExprStepper"; }

    /// The v1model "method" externs ported from the bmv2/v1model target.
    static const ExternMethodImpls<TofinoV1ModelExprStepper> V1MODEL_EXTERN_METHOD_IMPLS;

 public:
    TofinoV1ModelExprStepper(ExecutionState &state, AbstractSolver &solver,
                             const ProgramInfo &programInfo);

    void evalExternMethodCall(const ExternInfo &externInfo) override;
};

}  // namespace P4::P4Tools::Symbex::Tofino

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_V1MODEL_EXPR_STEPPER_H_ */
