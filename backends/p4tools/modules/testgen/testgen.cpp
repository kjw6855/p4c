#include "backends/p4tools/modules/testgen/testgen.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <memory>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "backends/p4tools/common/compiler/context.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "frontends/common/parser_options.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "lib/gc.h"
#include "lib/cstring.h"
#include "lib/error.h"

#include "backends/p4tools/modules/testgen/core/compiler_result.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/greedy_node_cov.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/random_backtrack.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/selected_branches.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/concolic_executor/concolic_executor.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/test_backend.h"
#include "backends/p4tools/modules/testgen/lib/test_framework.h"
#include "backends/p4tools/modules/testgen/async_server.h"
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/register.h"
#include "backends/p4tools/modules/testgen/toolname.h"
#include "backends/p4tools/modules/testgen/p4testgen.pb.h"

namespace P4Tools::P4Testgen {

using grpc::ServerBuilder;
using p4testgen::P4FuzzGuide;
using p4testgen::TestCase;

class GraphMidEnd : public PassManager {
 public:
    P4::ReferenceMap refMap;
    P4::TypeMap typeMap;
    IR::ToplevelBlock *toplevel = nullptr;

    explicit GraphMidEnd(ParserOptions &options);
    IR::ToplevelBlock *process(const IR::P4Program *&program) {
        program = program->apply(*this);
        return toplevel;
    }
};

GraphMidEnd::GraphMidEnd(ParserOptions &options) {
    bool isv1 = options.langVersion == ParserOptions::FrontendVersion::P4_14;
    refMap.setIsV1(isv1);
    auto evaluator = new P4::EvaluatorPass(&refMap, &typeMap);
    setName("GraphMidEnd");

    addPasses({
        evaluator,
        [this, evaluator]() { toplevel = evaluator->getToplevelBlock(); },
    });
}

namespace {

/// Pick the path selection algorithm for the symbolic executor.
SymbolicExecutor *pickExecutionEngine(const TestgenOptions &testgenOptions,
                                      const ProgramInfo &programInfo, AbstractSolver &solver) {
    const auto &pathSelectionPolicy = testgenOptions.pathSelectionPolicy;
    if (pathSelectionPolicy == PathSelectionPolicy::GreedyStmtCoverage) {
        return new GreedyNodeSelection(solver, programInfo);
    }
    if (pathSelectionPolicy == PathSelectionPolicy::RandomBacktrack) {
        return new RandomBacktrack(solver, programInfo);
    }
    if (!testgenOptions.selectedBranches.empty()) {
        std::string selectedBranchesStr = testgenOptions.selectedBranches;
        return new SelectedBranches(solver, programInfo, selectedBranchesStr);
    }
    return new DepthFirstSearch(solver, programInfo);
}

/// Analyse the results of the symbolic execution and generate diagnostic messages.
int postProcess(const TestgenOptions &testgenOptions, const TestBackEnd &testBackend) {
    // Do not print this warning if assertion mode is enabled.
    if (testBackend.getTestCount() == 0 && !testgenOptions.assertionModeEnabled) {
        ::warning(
            "Unable to generate tests with given inputs. Double-check provided options and "
            "parameters.\n");
    }
    if (testBackend.getCoverage() < testgenOptions.minCoverage) {
        ::error("The tests did not achieve requested coverage of %1%, the coverage is %2%.",
                testgenOptions.minCoverage, testBackend.getCoverage());
    }

    return ::errorCount() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

std::optional<AbstractTestList> generateAndCollectAbstractTests(
    const TestgenOptions &testgenOptions, const ProgramInfo &programInfo) {
    if (!testgenOptions.testBaseName.has_value()) {
        ::error(
            "Test collection requires a test name. No name was provided as part of the "
            "P4Testgen options.");
        return std::nullopt;
    }

    // The test name is the stem of the output base path.
    TestBackendConfiguration testBackendConfiguration{testgenOptions.testBaseName.value(),
                                                      testgenOptions.maxTests, std::nullopt,
                                                      testgenOptions.seed};
    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbolicExecutor = pickExecutionEngine(testgenOptions, programInfo, solver);

    // Each test back end has a different run function.
    auto *testBackend =
        TestgenTarget::getTestBackend(programInfo, testBackendConfiguration, *symbolicExecutor);

    // Define how to handle the final state for each test. This is target defined.
    // We delegate execution to the symbolic executor.
    symbolicExecutor->run([testBackend](auto &&finalState) {
        return testBackend->run(std::forward<decltype(finalState)>(finalState));
    });
    auto result = postProcess(testgenOptions, *testBackend);
    if (result != EXIT_SUCCESS) {
        return std::nullopt;
    }
    return testBackend->getTests();
}

int generateAndWriteAbstractTests(const TestgenOptions &testgenOptions,
                                  const ProgramInfo &programInfo) {
    std::filesystem::path testPath;
    /// If the test name is not provided, use the steam of the input file name as test name.
    if (testgenOptions.testBaseName.has_value()) {
        testPath = testgenOptions.testBaseName.value().c_str();
    } else if (!P4CContext::get().options().file.empty()) {
        testPath = P4CContext::get().options().file.stem();
    } else {
        ::error("Neither a file nor test base name was set. Can not infer a test name.");
    }

    // Create the directory, if the directory string is valid and if it does not exist.
    if (testgenOptions.outputDir.has_value()) {
        auto testDir = testgenOptions.outputDir.value();
        try {
            std::filesystem::create_directories(testDir);
        } catch (const std::exception &err) {
            ::error("Unable to create directory %1%: %2%", testDir.c_str(), err.what());
            return EXIT_FAILURE;
        }
        testPath = testDir / testPath;
    }

    // The test name is the stem of the output base path.
    TestBackendConfiguration testBackendConfiguration{
        cstring(testPath.c_str()), testgenOptions.maxTests, testPath, testgenOptions.seed};

    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbolicExecutor = pickExecutionEngine(testgenOptions, programInfo, solver);

    // Each test back end has a different run function.
    auto *testBackend =
        TestgenTarget::getTestBackend(programInfo, testBackendConfiguration, *symbolicExecutor);

    // Define how to handle the final state for each test. This is target defined.
    // We delegate execution to the symbolic executor.
    symbolicExecutor->run([testBackend](auto &&finalState) {
        return testBackend->run(std::forward<decltype(finalState)>(finalState));
    });
    return postProcess(testgenOptions, *testBackend);
}

std::optional<AbstractTestList> generateTestsImpl(std::optional<std::string_view> program,
                                                  const CompilerOptions &compilerOptions,
                                                  const TestgenOptions &testgenOptions,
                                                  bool writeTests) {
    P4Tools::Target::init(compilerOptions.target.c_str(), compilerOptions.arch.c_str());

    // Set up the compilation context.
    auto *compileContext = new CompileContext<CompilerOptions>();
    compileContext->options() = compilerOptions;
    AutoCompileContext autoContext(compileContext);
    CompilerResultOrError compilerResultOpt;
    if (program.has_value()) {
        // Run the compiler to get an IR and invoke the tool.
        compilerResultOpt =
            P4Tools::CompilerTarget::runCompiler(TOOL_NAME, std::string(program.value()));
    } else {
        if (compilerOptions.file.empty()) {
            ::error("Expected a file input.");
            return std::nullopt;
        }
        // Run the compiler to get an IR and invoke the tool.
        compilerResultOpt = P4Tools::CompilerTarget::runCompiler(TOOL_NAME);
    }

    if (!compilerResultOpt.has_value()) {
        ::error("Failed to run the compiler.");
        return std::nullopt;
    }

    const auto *testgenCompilerResult =
        compilerResultOpt.value().get().checkedTo<TestgenCompilerResult>();

    const auto *programInfo = TestgenTarget::produceProgramInfo(*testgenCompilerResult);
    if (programInfo == nullptr || ::errorCount() > 0) {
        ::error("P4Testgen encountered errors during preprocessing.");
        return std::nullopt;
    }

    if (writeTests) {
        int result = generateAndWriteAbstractTests(testgenOptions, *programInfo);
        if (result != EXIT_SUCCESS) {
            return std::nullopt;
        }
        return {};
    }
    return generateAndCollectAbstractTests(testgenOptions, *programInfo);
}

}  // namespace

void Testgen::runServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
        const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        int grpcPort) {
    std::string server_address("0.0.0.0:");
    ServerState state;
    server_address += std::to_string(grpcPort);

    std::map<std::string, ConcolicExecutor*> coverageMap;
    P4FuzzGuideImpl service = P4FuzzGuideImpl(coverageMap,
            *programInfo, tableCollector, top, refMap, typeMap, &state);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    server = builder.BuildAndStart();
    std::cout << "Server listening on " << server_address << std::endl;

    // Main thread loop to check the shutdown flag
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state.shutdown_mu);
            if (state.shutdown_requested)
                break;
        }
        // Sleep or perform other tasks, you can adjust this sleep duration
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server->Shutdown();
    server->Wait();
    std::cout << "Shutdown server gracefully" << std::endl;
}

void Testgen::registerTarget() {
    // Register all available P4Testgen targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerTestgenTargets();
}

int Testgen::mainImpl(const CompilerResult &compilerResult) {
    // Make sure the input result corresponds to the result we expect.
    const auto *testgenCompilerResult = compilerResult.checkedTo<TestgenCompilerResult>();

    const auto *programInfo = TestgenTarget::produceProgramInfo(*testgenCompilerResult);
    if (programInfo == nullptr || ::errorCount() > 0) {
        ::error("P4Testgen encountered errors during preprocessing.");
        return EXIT_FAILURE;
    }
    const auto &testgenOptions = TestgenOptions::get();

    if (testgenOptions.interactive) {
        auto &options = P4CContext::get().options();
        GraphMidEnd midEnd(options);
        midEnd.addDebugHook(options.getDebugHook());
        const IR::ToplevelBlock *top = nullptr;
        const auto *program = &programInfo->getP4Program();
        try {
            top = midEnd.process(program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }

        // Get Tables and Actions
        auto tableCollector = TableCollector();
        program->apply(tableCollector);
        tableCollector.findP4Actions();

        auto p4Tables = tableCollector.getP4Tables();
        auto p4TableActions = tableCollector.getActionNodes();
        LOG_FEATURE("small_visit", 4, "Table/Action size: " << p4Tables.size() << "/" << p4TableActions.size());

        for (const auto *table : tableCollector.getP4TableSet()) {
            LOG_FEATURE("small_visit", 4, "  [T] " << table->controlPlaneName());
        }

        LOG_FEATURE("small_visit", 4, "============================================");
        for (auto *action : p4TableActions) {
            const auto &srcInfo = action->getSourceInfo();
            auto sourceLine = srcInfo.toPosition().sourceLine;
            LOG_FEATURE("small_visit", 4, "  [A] " << srcInfo.getSourceFile() <<
                    "\\" << sourceLine << ": " << *action);
        }

        runServer(programInfo, tableCollector, top,
                &midEnd.refMap, &midEnd.typeMap, testgenOptions.grpcPort);
        return EXIT_SUCCESS;
    }

    if (testgenOptions.pathSelectionPolicy == PathSelectionPolicy::TestCase) {
        auto &options = P4CContext::get().options();
        GraphMidEnd midEnd(options);
        midEnd.addDebugHook(options.getDebugHook());
        const IR::ToplevelBlock *top = nullptr;
        const auto *program = &programInfo->getP4Program();
        try {
            top = midEnd.process(program);
        } catch (const std::exception &bug) {
            std::cerr << bug.what() << std::endl;
            return 1;
        }

        auto tableCollector = TableCollector();
        program->apply(tableCollector);
        auto *concExec = new ConcolicExecutor(*programInfo, tableCollector, top,
                &midEnd.refMap, &midEnd.typeMap);
        TestCase *testCase = new TestCase();
        concExec->setGenRuleMode(false);
        int fd = open("/home/jwkim/Workspace-remote/p4testgen_out/latest/basic2/basic._4.proto", O_RDONLY);

        if (fd < 0) {
            std::cerr << " Error opening the file " << std::endl;
        }

        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete( true );

        if (!google::protobuf::TextFormat::Parse(&fileInput, testCase)) {
            std::cerr << std::endl << "Failed to parse file!" << std::endl;
        } else {
            std::cerr << "Read Input File" << std::endl;
        }
        concExec->run(*testCase);
        return EXIT_SUCCESS;
    }

    return generateAndWriteAbstractTests(testgenOptions, *programInfo);
}

std::optional<AbstractTestList> Testgen::generateTests(std::string_view program,
                                                       const CompilerOptions &compilerOptions,
                                                       const TestgenOptions &testgenOptions) {
    try {
        return generateTestsImpl(program, compilerOptions, testgenOptions, false);
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << "\n";
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<AbstractTestList> Testgen::generateTests(const CompilerOptions &compilerOptions,
                                                       const TestgenOptions &testgenOptions) {
    try {
        return generateTestsImpl(std::nullopt, compilerOptions, testgenOptions, false);
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << "\n";
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

int Testgen::writeTests(std::string_view program, const CompilerOptions &compilerOptions,
                        const TestgenOptions &testgenOptions) {
    try {
        if (generateTestsImpl(program, compilerOptions, testgenOptions, true).has_value()) {
            return EXIT_SUCCESS;
        }
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}

}  // namespace P4Tools::P4Testgen
