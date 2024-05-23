#include "backends/p4tools/modules/testgen/testgen.h"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <memory>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/util.h"
#include "frontends/common/parser_options.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "lib/gc.h"
#include "lib/cstring.h"
#include "lib/error.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/greedy_stmt_cov.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/max_stmt_cov.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/random_backtrack.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/selected_branches.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/concolic_executor/concolic_executor.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/logging.h"
#include "backends/p4tools/modules/testgen/lib/test_backend.h"
#include "backends/p4tools/modules/testgen/async_server.h"
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/register.h"
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

Testgen::~Testgen() {
    if (server_ != nullptr)
        server_->Shutdown();
    if (cq_ != nullptr)
        cq_->Shutdown();
}

void Testgen::runServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
        const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        int grpcPort) {
    std::string server_address("0.0.0.0:");
    server_address += std::to_string(grpcPort);

    //P4FuzzGuide::AsyncService service_;
    std::map<std::string, ConcolicExecutor*> coverageMap;
    P4FuzzGuideImpl service = P4FuzzGuideImpl(coverageMap,
            programInfo, tableCollector, top, refMap, typeMap);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    //cq_ = builder.AddCompletionQueue();
    //server_ = builder.BuildAndStart();

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}

void Testgen::registerTarget() {
    // Register all available compiler targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerCompilerTargets();
}

SymbolicExecutor *pickExecutionEngine(const TestgenOptions &testgenOptions,
                                      const ProgramInfo *programInfo, AbstractSolver &solver) {
    const auto &pathSelectionPolicy = testgenOptions.pathSelectionPolicy;
    if (pathSelectionPolicy == PathSelectionPolicy::GreedyStmtCoverage) {
        return new GreedyStmtSelection(solver, *programInfo);
    }
    if (pathSelectionPolicy == PathSelectionPolicy::RandomBacktrack) {
        return new RandomBacktrack(solver, *programInfo);
    }
    if (pathSelectionPolicy == PathSelectionPolicy::RandomMaxStmtCoverage) {
        return new RandomMaxStmtCoverage(solver, *programInfo, testgenOptions.saddlePoint);
    }
    if (!testgenOptions.selectedBranches.empty()) {
        std::string selectedBranchesStr = testgenOptions.selectedBranches;
        return new SelectedBranches(solver, *programInfo, selectedBranchesStr);
    }
    return new DepthFirstSearch(solver, *programInfo);
}

int generateAbstractTests(const TestgenOptions &testgenOptions, const ProgramInfo *programInfo,
                          SymbolicExecutor &symbex) {
    // Get the filename of the input file and remove the extension
    // This assumes that inputFile is not null.
    auto const inputFile = P4CContext::get().options().file;
    auto testPath = std::filesystem::path(inputFile.c_str()).stem();
    // Create the directory, if the directory string is valid and if it does not exist.
    cstring testDirStr = testgenOptions.outputDir;
    if (!testDirStr.isNullOrEmpty()) {
        auto testDir = std::filesystem::path(testDirStr.c_str());
        std::filesystem::create_directories(testDir);
        testPath = testDir / testPath;
    }
    // Each test back end has a different run function.
    auto *testBackend = TestgenTarget::getTestBackend(*programInfo, symbex, testPath);
    // Define how to handle the final state for each test. This is target defined.
    // We delegate execution to the symbolic executor.
    auto callBack = [testBackend](auto &&finalState) {
        return testBackend->run(std::forward<decltype(finalState)>(finalState));
    };

    // Run the symbolic executor with given exploration strategy.
    symbex.run(callBack);

    // Emit a performance report, if desired.
    testBackend->printPerformanceReport(true);

    // Do not print this warning if assertion mode is enabled.
    if (testBackend->getTestCount() == 0 && !testgenOptions.assertionModeEnabled) {
        ::warning(
            "Unable to generate tests with given inputs. Double-check provided options and "
            "parameters.\n");
    }
    return ::errorCount() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int Testgen::mainImpl(const IR::P4Program *program) {
    // Register all available testgen targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerTestgenTargets();

    const auto *programInfo = TestgenTarget::initProgram(program);
    if (programInfo == nullptr) {
        ::error("Program not supported by target device and architecture.");
        return EXIT_FAILURE;
    }
    if (::errorCount() > 0) {
        ::error("Testgen: Encountered errors during preprocessing. Exiting");
        return EXIT_FAILURE;
    }

    // Print basic information for each test.
    enableInformationLogging();

    // Get the options and the seed.
    const auto &testgenOptions = TestgenOptions::get();
    auto seed = Utils::getCurrentSeed();
    if (seed) {
        printFeature("test_info", 4, "============ Program seed %1% =============\n", *seed);
    }

    auto &options = P4CContext::get().options();
    GraphMidEnd midEnd(options);
    midEnd.addDebugHook(options.getDebugHook());
    const IR::ToplevelBlock *top = nullptr;
    try {
        top = midEnd.process(program);
    } catch (const std::exception &bug) {
        std::cerr << bug.what() << std::endl;
        return 1;
    }

    if (testgenOptions.interactive) {
        // Get Tables and Actions
        auto tableCollector = TableCollector();
        programInfo->program->apply(tableCollector);
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
        auto tableCollector = TableCollector();
        programInfo->program->apply(tableCollector);
        auto *concExec = new ConcolicExecutor(*programInfo, tableCollector, top,
                &midEnd.refMap, &midEnd.typeMap);
        TestCase *testCase = new TestCase();
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

    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbex = pickExecutionEngine(testgenOptions, programInfo, solver);

    return generateAbstractTests(testgenOptions, programInfo, *symbex);
}

}  // namespace P4Tools::P4Testgen
