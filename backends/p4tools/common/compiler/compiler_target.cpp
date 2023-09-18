#include "backends/p4tools/common/compiler/compiler_target.h"

#include <string>
#include <utility>
#include <vector>

#include "backends/p4tools/common/compiler/context.h"
#include "backends/p4tools/common/compiler/convert_hs_index.h"
#include "backends/p4tools/common/compiler/midend.h"
#include "backends/p4tools/common/core/target.h"
#include "frontends/common/applyOptionsPragmas.h"
#include "frontends/common/options.h"
#include "frontends/common/parseInput.h"
#include "frontends/common/parser_options.h"
#include "frontends/p4/frontend.h"
#include "fstream"
#include "ir/json_loader.h"
#include "lib/compile_context.h"
#include "lib/error.h"

namespace P4Tools {

ICompileContext *CompilerTarget::makeContext() { return get().makeContextImpl(); }

std::vector<const char *> *CompilerTarget::initCompiler(int argc, char **argv) {
    return get().initCompilerImpl(argc, argv);
}

std::optional<const IR::P4Program *> CompilerTarget::runCompiler() {
    const auto *program = P4Tools::CompilerTarget::runParser();
    if (program == nullptr) {
        return std::nullopt;
    }

    return runCompiler(program);
}

std::optional<const IR::P4Program *> CompilerTarget::loadProgram(cstring irJsonFile) {
    std::filebuf fb;
    auto &options = P4CContext::get().options();

    if (fb.open(irJsonFile, std::ios::in) == nullptr) {
        ::error(ErrorType::ERR_IO, "%s: No such file or directory.", irJsonFile);
        return std::nullopt;
    }
    std::istream inJson(&fb);
    JSONLoader jsonFileLoader(inJson);
    if (jsonFileLoader.json == nullptr) {
        ::error(ErrorType::ERR_IO, "%s: Not valid json input file", irJsonFile);
        return std::nullopt;
    }
    const auto *program = new IR::P4Program(jsonFileLoader);
    fb.close();

    auto &compilerOptions = dynamic_cast<CompilerOptions &>(options);
    P4::serializeP4RuntimeIfRequired(program, compilerOptions);
    if (::errorCount() > 0) return std::nullopt;

    const auto &self = get();
    program = self.runMidEnd(program, true);
    if (program == nullptr) {
        return std::nullopt;
    }

    program = program->apply(HSIndexToMember());

    return program;
}

std::optional<const IR::P4Program *> CompilerTarget::runCompiler(const std::string &source) {
    const auto *program = P4::parseP4String(source, P4CContext::get().options().langVersion);
    if (program == nullptr) {
        return std::nullopt;
    }

    return runCompiler(program);
}

std::optional<const IR::P4Program *> CompilerTarget::runCompiler(const IR::P4Program *program) {
    return get().runCompilerImpl(program);
}

std::optional<const IR::P4Program *> CompilerTarget::runCompilerImpl(
    const IR::P4Program *program) const {
    const auto &self = get();

    program = self.runFrontend(program);
    if (program == nullptr) {
        return std::nullopt;
    }

    program = self.runMidEnd(program, false);
    if (program == nullptr) {
        return std::nullopt;
    }

    // Rewrite all occurrences of ArrayIndex to be members instead.
    // IMPORTANT: After this change, the program will no longer type-check.
    // This is why perform this rewrite after all front and mid end passes have been applied.
    program = program->apply(HSIndexToMember());

    return program;
}

ICompileContext *CompilerTarget::makeContextImpl() const {
    return new CompileContext<CompilerOptions>();
}

std::vector<const char *> *CompilerTarget::initCompilerImpl(int argc, char **argv) const {
    auto *result = P4CContext::get().options().process(argc, argv);
    return ::errorCount() > 0 ? nullptr : result;
}

const IR::P4Program *CompilerTarget::runParser() {
    auto &options = P4CContext::get().options();

    const auto *program = P4::parseP4File(options);
    if (::errorCount() > 0) {
        return nullptr;
    }
    return program;
}

const IR::P4Program *CompilerTarget::runFrontend(const IR::P4Program *program) const {
    // Dynamic cast to get the CompilerOptions from ParserOptions
    auto &options = dynamic_cast<CompilerOptions &>(P4CContext::get().options());

    P4::P4COptionPragmaParser optionsPragmaParser;
    program->apply(P4::ApplyOptionsPragmas(optionsPragmaParser));

    auto frontEnd = mkFrontEnd();
    frontEnd.addDebugHook(options.getDebugHook());
    program = frontEnd.run(options, program);
    if ((program == nullptr) || ::errorCount() > 0) {
        return nullptr;
    }
    return program;
}

P4::FrontEnd CompilerTarget::mkFrontEnd() const { return {}; }

MidEnd CompilerTarget::mkMidEnd(const CompilerOptions &options, bool loadIRFromJson) const {
    MidEnd midEnd(options);
    midEnd.addDefaultPasses(loadIRFromJson);
    return midEnd;
}

const IR::P4Program *CompilerTarget::runMidEnd(const IR::P4Program *program, bool loadIRFromJson) const {
    // Dynamic cast to get the CompilerOptions from ParserOptions
    auto &options = dynamic_cast<CompilerOptions &>(P4CContext::get().options());

    auto midEnd = mkMidEnd(options, loadIRFromJson);
    midEnd.addDebugHook(options.getDebugHook(), true);
    return program->apply(midEnd);
}

CompilerTarget::CompilerTarget(std::string deviceName, std::string archName)
    : Target("compiler", std::move(deviceName), std::move(archName)) {}

const CompilerTarget &CompilerTarget::get() { return Target::get<CompilerTarget>("compiler"); }

}  // namespace P4Tools
