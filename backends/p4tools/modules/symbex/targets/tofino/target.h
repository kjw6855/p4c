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

#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TARGET_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TARGET_H_

#include <string>

#include <boost/filesystem.hpp>

#include "ir/ir.h"
#include "ir/solver.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/tofino/test_backend.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino/program_info.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino2/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino2/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino2/program_info.h"
#include "backends/p4tools/modules/symbex/targets/tofino/v1model/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/v1model/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/v1model/program_info.h"
#if HAVE_FLATROCK_TARGET
#include "backends/p4tools/modules/symbex/targets/tofino/tofino5/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/tofino/tofino5/expr_stepper.h"
#endif

namespace P4::P4Tools::Symbex::Tofino {

class AbstractTofinoProgramInfo : public ProgramInfo {
 public:
    ~AbstractTofinoProgramInfo() override = default;
};

class AbstractTofinoSymbexTarget : public SymbexTarget {
 protected:
    TofinoTestBackend *getTestBackendImpl(const ProgramInfo &programInfo,
                                          const TestBackendConfiguration &testBackendConfiguration,
                                          SymbolicExecutor &symbex) const override;

    explicit AbstractTofinoSymbexTarget(const std::string &deviceName,
                                         const std::string &archName);

    [[nodiscard]] MidEnd mkMidEnd(const CompilerOptions &options) const override;

    [[nodiscard]] P4::FrontEnd mkFrontEnd() const override;

    CompilerResultOrError runCompilerImpl(const CompilerOptions &options,
                                          const IR::P4Program *program) const override;
};

class Tofino_TnaSymbexTarget : public AbstractTofinoSymbexTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const TofinoProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult,
        const IR::Declaration_Instance *mainDecl) const override;

    TofinoCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                        const ProgramInfo &programInfo) const override;

    Tofino1ExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                           const ProgramInfo &programInfo) const override;

    CmdVisitor *getCmdVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                  TestCase &testCase) const override;

    ExprVisitor *getExprVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                    TestCase &testCase) const override;

 private:
    Tofino_TnaSymbexTarget();
};

class JBay_T2naSymbexTarget : public AbstractTofinoSymbexTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const JBayProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult,
        const IR::Declaration_Instance *mainDecl) const override;

    JBayCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                      const ProgramInfo &programInfo) const override;

    JBayExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                        const ProgramInfo &programInfo) const override;

    CmdVisitor *getCmdVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                  TestCase &testCase) const override;

    ExprVisitor *getExprVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                    TestCase &testCase) const override;

 private:
    JBay_T2naSymbexTarget();
};

/// The (tofino, v1model) target: a v1model `V1Switch` program (p4_14 -> p4_16 conversion output)
/// that uses native Tofino RegisterAction externs. Reuses the Tofino device's expr stepper, BFRT
/// backend, and compiler config; supplies a V1Switch-shaped program info and a standard_metadata
/// command stepper.
class Tofino_V1ModelSymbexTarget : public AbstractTofinoSymbexTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const TofinoV1ModelProgramInfo *produceProgramInfoImpl(
        const CompilerResult &compilerResult,
        const IR::Declaration_Instance *mainDecl) const override;

    TofinoV1ModelCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                               const ProgramInfo &programInfo) const override;

    TofinoV1ModelExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                                 const ProgramInfo &programInfo) const override;

    CmdVisitor *getCmdVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                  TestCase &testCase) const override;

    ExprVisitor *getExprVisitorImpl(ExecutionState &state, const ProgramInfo &programInfo,
                                    TestCase &testCase) const override;

 private:
    Tofino_V1ModelSymbexTarget();
};

}  // namespace P4::P4Tools::Symbex::Tofino

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_TOFINO_TARGET_H_ */
