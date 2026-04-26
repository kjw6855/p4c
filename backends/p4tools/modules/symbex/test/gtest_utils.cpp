#include "backends/p4tools/modules/symbex/test/gtest_utils.h"

#include <optional>

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/compiler/context.h"
#include "backends/p4tools/common/core/target.h"
#include "backends/p4tools/common/lib/variables.h"
#include "frontends/common/options.h"
#include "frontends/common/parser_options.h"
#include "lib/compile_context.h"
#include "lib/exceptions.h"

#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/register.h"
#include "backends/p4tools/modules/symbex/toolname.h"

namespace P4::P4Tools::Test {

P4ToolsTestCase::P4ToolsTestCase(const P4Tools::CompilerResult &compilerResults)
    : compilerResults(compilerResults) {}

std::optional<const P4ToolsTestCase> P4ToolsTestCase::create(
    std::string deviceName, std::string archName, CompilerOptions::FrontendVersion langVersion,
    const std::string &source) {
    // Initialize the target.
    ensureInit();
    BUG_CHECK(P4Tools::Target::init(deviceName, archName), "Target %1%/%2% not supported",
              deviceName, archName);

    // Set up the compilation context and set the source language.
    auto context =
        P4Tools::Target::initializeTarget(P4::P4Tools::Symbex::TOOL_NAME, deviceName, archName);
    if (!context.has_value()) {
        return std::nullopt;
    }
    AutoCompileContext autoContext(context.value());
    auto *compileContext =
        dynamic_cast<P4Tools::CompileContext<Symbex::SymbexOptions> *>(context.value());
    compileContext->options().langVersion = langVersion;

    auto compilerResults = P4Tools::CompilerTarget::runCompiler(
        compileContext->options(), P4::P4Tools::Symbex::TOOL_NAME, source);
    if (!compilerResults.has_value()) {
        return std::nullopt;
    }
    return P4ToolsTestCase(compilerResults.value());
}

const IR::P4Program &P4ToolsTestCase::getProgram() const {
    return getCompilerResult().getProgram();
}

const P4Tools::CompilerResult &P4ToolsTestCase::getCompilerResult() const {
    return compilerResults;
}

std::optional<const P4ToolsTestCase> P4ToolsTestCase::create_14(std::string deviceName,
                                                                std::string archName,
                                                                const std::string &source) {
    return create(deviceName, archName, CompilerOptions::FrontendVersion::P4_14, source);
}

std::optional<const P4ToolsTestCase> P4ToolsTestCase::create_16(std::string deviceName,
                                                                std::string archName,
                                                                const std::string &source) {
    return create(deviceName, archName, CompilerOptions::FrontendVersion::P4_16, source);
}

void P4ToolsTestCase::ensureInit() {
    static bool INITIALIZED = false;
    if (INITIALIZED) {
        return;
    }
    // Register supported Symbex targets.
    P4::P4Tools::Symbex::registerSymbexTargets();

    INITIALIZED = true;
}

const IR::SymbolicVariable *SymbolicConverter::preorder(IR::Member *member) {
    return P4Tools::ToolsVariables::getSymbolicVariable(member->type, member->toString());
}

}  // namespace P4::P4Tools::Test
