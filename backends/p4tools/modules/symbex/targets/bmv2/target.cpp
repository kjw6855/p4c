#include "backends/p4tools/modules/symbex/targets/bmv2/target.h"

#include <cstddef>
#include <map>
#include <vector>

#include "backends/bmv2/common/annotations.h"
#include "backends/p4tools/common/lib/util.h"
#include "ir/ir.h"
#include "ir/solver.h"
#include "lib/cstring.h"
#include "lib/error_catalog.h"
#include "lib/exceptions.h"
#include "lib/ordered_map.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/cmd_visitor.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/compiler_result.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/constants.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/expr_stepper.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/expr_visitor.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/p4_refers_to_parser.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/p4runtime_translation.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/program_info.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/test_backend.h"

namespace P4::P4Tools::Symbex::Bmv2 {

/* =============================================================================================
 *  Bmv2V1ModelSymbexTarget implementation
 * ============================================================================================= */

Bmv2V1ModelSymbexTarget::Bmv2V1ModelSymbexTarget() : SymbexTarget("bmv2", "v1model") {}

void Bmv2V1ModelSymbexTarget::make() {
    static Bmv2V1ModelSymbexTarget *INSTANCE = nullptr;
    if (INSTANCE == nullptr) {
        INSTANCE = new Bmv2V1ModelSymbexTarget();
    }
}

CompilerResultOrError Bmv2V1ModelSymbexTarget::runCompilerImpl(
    const CompilerOptions &options, const IR::P4Program *program) const {
    program = runFrontend(options, program);
    if (program == nullptr) {
        return std::nullopt;
    }

    /// After the front end, get the P4Runtime API for the V1model architecture.
    auto p4runtimeApi = P4::P4RuntimeSerializer::get()->generateP4Runtime(program, "v1model"_cs);

    if (errorCount() > 0) {
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
    if (errorCount() > 0) {
        return std::nullopt;
    }
    /// Collect coverage information about the program.
    auto coverage = P4::Coverage::CollectNodes(SymbexOptions::get().coverageOptions);
    program->apply(coverage);
    if (errorCount() > 0) {
        return std::nullopt;
    }

    // Parses any @refers_to annotations and converts them into a vector of restrictions.
    auto refersToParser = RefersToParser();
    program->apply(refersToParser);
    if (errorCount() > 0) {
        return std::nullopt;
    }
    ConstraintsVector p4ConstraintsRestrictions = refersToParser.getRestrictionsVector();

    // Defines all "entry_restriction" and then converts restrictions from string to IR
    // expressions, and stores them in p4ConstraintsRestrictions to move targetConstraints
    // further.
    program->apply(AssertsParser(p4ConstraintsRestrictions));
    if (errorCount() > 0) {
        return std::nullopt;
    }
    // Try to map all instances of direct externs to the table they are attached to.
    // Save the map in @var directExternMap.
    auto directExternMapper = MapDirectExterns();
    program->apply(directExternMapper);
    if (errorCount() > 0) {
        return std::nullopt;
    }

    return {*new BMv2V1ModelCompilerResult{
        SymbexCompilerResult(CompilerResult(*program), coverage.getCoverableNodes(), dcg),
        p4runtimeApi, directExternMapper.getDirectExternMap(), p4ConstraintsRestrictions}};
}

MidEnd Bmv2V1ModelSymbexTarget::mkMidEnd(const CompilerOptions &options) const {
    MidEnd midEnd(options);
    auto *refMap = midEnd.getRefMap();
    auto *typeMap = midEnd.getTypeMap();
    midEnd.addPasses({
        // Parse BMv2-specific annotations.
        new BMV2::ParseAnnotations(),
        new P4::TypeChecking(refMap, typeMap, true),
        new PropagateP4RuntimeTranslation(*typeMap),
    });
    midEnd.addDefaultPasses();

    return midEnd;
}

const Bmv2V1ModelProgramInfo *Bmv2V1ModelSymbexTarget::produceProgramInfoImpl(
    const CompilerResult &compilerResult, const IR::Declaration_Instance *mainDecl) const {
    const auto *mainType = mainDecl->type->to<IR::Type_Specialized>();
    if (mainType == nullptr || mainType->baseType->path->name != "V1Switch") {
        error(ErrorType::ERR_INVALID,
              "%1%: This Symbex back end only supports a 'V1Switch' main package. The current "
              "type is %2%",
              mainDecl, mainDecl->type);
        return nullptr;
    }
    // The blocks in the main declaration are just the arguments in the constructor call.
    // Convert mainDecl->arguments into a vector of blocks, represented as constructor-call
    // expressions.
    const auto blocks =
        argumentsToTypeDeclarations(&compilerResult.getProgram(), mainDecl->arguments);

    // We should have six arguments.
    if (blocks.size() != 6) {
        error(ErrorType::ERR_INVALID, "%1%: The BMV2 architecture requires 6 pipes. Received %2%.",
              mainDecl, blocks.size());
        return nullptr;
    }

    ordered_map<cstring, const IR::Type_Declaration *> programmableBlocks;
    std::map<int, int> declIdToGress;

    // Add to parserDeclIdToGress, mauDeclIdToGress, and deparserDeclIdToGress.
    for (size_t idx = 0; idx < blocks.size(); ++idx) {
        const auto *declType = blocks.at(idx);

        auto canonicalName = Bmv2V1ModelProgramInfo::ARCH_SPEC.getArchMember(idx)->blockName;
        programmableBlocks.emplace(canonicalName, declType);

        if (idx < 3) {
            declIdToGress[declType->declid] = BMV2_INGRESS;
        } else {
            declIdToGress[declType->declid] = BMV2_EGRESS;
        }
    }

    return new Bmv2V1ModelProgramInfo(*compilerResult.checkedTo<BMv2V1ModelCompilerResult>(),
                                      programmableBlocks, declIdToGress);
}

Bmv2TestBackend *Bmv2V1ModelSymbexTarget::getTestBackendImpl(
    const ProgramInfo &programInfo, const TestBackendConfiguration &testBackendConfiguration,
    SymbolicExecutor &symbex) const {
    return new Bmv2TestBackend(*programInfo.checkedTo<Bmv2V1ModelProgramInfo>(),
                               testBackendConfiguration, symbex);
}

Bmv2V1ModelCmdStepper *Bmv2V1ModelSymbexTarget::getCmdStepperImpl(
    ExecutionState &state, AbstractSolver &solver, const ProgramInfo &programInfo) const {
    return new Bmv2V1ModelCmdStepper(state, solver, programInfo);
}

Bmv2V1ModelExprStepper *Bmv2V1ModelSymbexTarget::getExprStepperImpl(
    ExecutionState &state, AbstractSolver &solver, const ProgramInfo &programInfo) const {
    return new Bmv2V1ModelExprStepper(state, solver, programInfo);
}

Bmv2V1ModelCmdVisitor *Bmv2V1ModelSymbexTarget::getCmdVisitorImpl(
    ExecutionState &state, const ProgramInfo &programInfo, TestCase &testCase) const {
    return new Bmv2V1ModelCmdVisitor(state, programInfo, testCase);
}

Bmv2V1ModelExprVisitor *Bmv2V1ModelSymbexTarget::getExprVisitorImpl(
    ExecutionState &state, const ProgramInfo &programInfo, TestCase &testCase) const {
    return new Bmv2V1ModelExprVisitor(state, programInfo, testCase);
}

}  // namespace P4::P4Tools::Symbex::Bmv2
