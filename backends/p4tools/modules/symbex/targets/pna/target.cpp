#include "backends/p4tools/modules/symbex/targets/pna/target.h"

#include <cstddef>
#include <vector>

#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "ir/solver.h"
#include "lib/cstring.h"
#include "lib/exceptions.h"
#include "lib/ordered_map.h"

#include "backends/p4tools/modules/symbex/core/compiler_result.h"
#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/pna/dpdk/program_info.h"
#include "backends/p4tools/modules/symbex/targets/pna/test_backend.h"

namespace P4::P4Tools::Symbex::Pna {

/* =============================================================================================
 *  PnaDpdkSymbexTarget implementation
 * ============================================================================================= */

PnaDpdkSymbexTarget::PnaDpdkSymbexTarget() : SymbexTarget("dpdk", "pna") {}

void PnaDpdkSymbexTarget::make() {
    static PnaDpdkSymbexTarget *INSTANCE = nullptr;
    if (INSTANCE == nullptr) {
        INSTANCE = new PnaDpdkSymbexTarget();
    }
}

const PnaDpdkProgramInfo *PnaDpdkSymbexTarget::produceProgramInfoImpl(
    const CompilerResult &compilerResult, const IR::Declaration_Instance *mainDecl) const {
    const auto *mainType = mainDecl->type->to<IR::Type_Specialized>();
    if (mainType == nullptr || mainType->baseType->path->name != "PNA_NIC") {
        error(ErrorType::ERR_INVALID,
              "%1%: This Symbex back end only supports a 'PNA_NIC' main package. The current "
              "type is %2%",
              mainDecl, mainDecl->type);
        return nullptr;
    }
    // The blocks in the main declaration are just the arguments in the constructor call.
    // Convert mainDecl->arguments into a vector of blocks, represented as constructor-call
    // expressions.
    const auto blocks =
        argumentsToTypeDeclarations(&compilerResult.getProgram(), mainDecl->arguments);

    // We should have four arguments.
    if (blocks.size() != 4) {
        error(ErrorType::ERR_INVALID, "%1%: The PNA architecture requires 4 pipes. Received %2%.",
              mainDecl, blocks.size());
        return nullptr;
    }

    ordered_map<cstring, const IR::Type_Declaration *> programmableBlocks;
    // Add to parserDeclIdToGress, mauDeclIdToGress, and deparserDeclIdToGress.
    for (size_t idx = 0; idx < blocks.size(); ++idx) {
        const auto *declType = blocks.at(idx);

        auto canonicalName = PnaDpdkProgramInfo::ARCH_SPEC.getArchMember(idx)->blockName;
        programmableBlocks.emplace(canonicalName, declType);
    }

    return new PnaDpdkProgramInfo(*compilerResult.checkedTo<SymbexCompilerResult>(),
                                  programmableBlocks);
}

PnaTestBackend *PnaDpdkSymbexTarget::getTestBackendImpl(
    const ProgramInfo &programInfo, const TestBackendConfiguration &testBackendConfiguration,
    SymbolicExecutor &symbex) const {
    return new PnaTestBackend(programInfo, testBackendConfiguration, symbex);
}

PnaDpdkCmdStepper *PnaDpdkSymbexTarget::getCmdStepperImpl(ExecutionState &state,
                                                           AbstractSolver &solver,
                                                           const ProgramInfo &programInfo) const {
    return new PnaDpdkCmdStepper(state, solver, programInfo);
}

PnaDpdkExprStepper *PnaDpdkSymbexTarget::getExprStepperImpl(ExecutionState &state,
                                                             AbstractSolver &solver,
                                                             const ProgramInfo &programInfo) const {
    return new PnaDpdkExprStepper(state, solver, programInfo);
}

CmdVisitor *PnaDpdkSymbexTarget::getCmdVisitorImpl(ExecutionState &state,
                                                           const ProgramInfo &programInfo,
                                                           TestCase &testCase) const {
    // TODO
    return nullptr;
}

ExprVisitor *PnaDpdkSymbexTarget::getExprVisitorImpl(ExecutionState &state,
                                                             const ProgramInfo &programInfo,
                                                             TestCase &testCase) const {
    // TODO
    return nullptr;
}

MidEnd PnaDpdkSymbexTarget::mkMidEnd(const CompilerOptions &options) const {
    MidEnd midEnd(options);
    midEnd.addDefaultPasses();

    return midEnd;
}

}  // namespace P4::P4Tools::Symbex::Pna
