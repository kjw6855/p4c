#include "backends/p4tools/modules/symbex/core/target.h"

#include <string>

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/compiler/context.h"
#include "backends/p4tools/common/core/target.h"
#include "ir/declaration.h"
#include "ir/ir.h"
#include "ir/node.h"
#include "lib/enumerator.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/toolname.h"

namespace P4::P4Tools::Symbex {

SymbexTarget::SymbexTarget(const std::string &deviceName, const std::string &archName)
    : CompilerTarget(TOOL_NAME, deviceName, archName) {}

const ProgramInfo *SymbexTarget::produceProgramInfoImpl(
    const CompilerResult &compilerResult) const {
    const auto &program = compilerResult.getProgram();
    // Check that the program has at least one main declaration.
    const auto mainCount = program.getDeclsByName(IR::P4Program::main)->count();
    BUG_CHECK(mainCount > 0, "Program doesn't have a main declaration.");

    // Resolve the program's main declaration instance and delegate to the version of
    // produceProgramInfoImpl that takes the main declaration.
    const auto *mainIDecl = program.getDeclsByName(IR::P4Program::main)->single();
    BUG_CHECK(mainIDecl, "Program's main declaration not found: %1%", program.main);

    const auto *mainNode = mainIDecl->getNode();
    const auto *mainDecl = mainIDecl->to<IR::Declaration_Instance>();
    BUG_CHECK(mainDecl, "%1%: Program's main declaration is a %2%, not a Declaration_Instance",
              mainNode, mainNode->node_type_name());

    return produceProgramInfoImpl(compilerResult, mainDecl);
}

const SymbexTarget &SymbexTarget::get() { return Target::get<SymbexTarget>("symbex"); }

TestBackEnd *SymbexTarget::getTestBackend(const ProgramInfo &programInfo,
                                           const TestBackendConfiguration &testBackendConfiguration,
                                           SymbolicExecutor &symbex) {
    return get().getTestBackendImpl(programInfo, testBackendConfiguration, symbex);
}

const ProgramInfo *SymbexTarget::produceProgramInfo(const CompilerResult &compilerResult) {
    return get().produceProgramInfoImpl(compilerResult);
}

ExprStepper *SymbexTarget::getExprStepper(ExecutionState &state, AbstractSolver &solver,
                                           const ProgramInfo &programInfo) {
    return get().getExprStepperImpl(state, solver, programInfo);
}

CmdStepper *SymbexTarget::getCmdStepper(ExecutionState &state, AbstractSolver &solver,
                                         const ProgramInfo &programInfo) {
    return get().getCmdStepperImpl(state, solver, programInfo);
}

ExprVisitor *SymbexTarget::getExprVisitor(ExecutionState &state,
                                         const ProgramInfo &programInfo, TestCase &testCase) {
    return get().getExprVisitorImpl(state, programInfo, testCase);
}

CmdVisitor *SymbexTarget::getCmdVisitor(ExecutionState &state,
                                         const ProgramInfo &programInfo, TestCase &testCase) {
    return get().getCmdVisitorImpl(state, programInfo, testCase);
}

CompilerResultOrError SymbexTarget::runCompilerImpl(const CompilerOptions &options,
                                                     const IR::P4Program *program) const {
    program = runFrontend(options, program);
    if (program == nullptr) {
        return std::nullopt;
    }

    program = runMidEnd(options, program);
    if (program == nullptr) {
        return std::nullopt;
    }

    // Create DCG.
    NodesCallGraph *dcg = nullptr;
    if (SymbexOptions::get().dcg || !SymbexOptions::get().pattern.empty()) {
        dcg = new NodesCallGraph("NodesCallGraph");
        P4ProgramDCGCreator dcgCreator(dcg);
        program->apply(dcgCreator);
    }

    /// Collect coverage information about the program.
    auto coverage = P4::Coverage::CollectNodes(SymbexOptions::get().coverageOptions);
    program->apply(coverage);

    return {
        *new SymbexCompilerResult(CompilerResult(*program), coverage.getCoverableNodes(), dcg)};
}

ICompileContext *SymbexTarget::makeContext() const {
    return new P4Tools::CompileContext<SymbexOptions>();
}

}  // namespace P4::P4Tools::Symbex
