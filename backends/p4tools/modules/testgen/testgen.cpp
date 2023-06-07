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

Testgen::~Testgen() {
    server_->Shutdown();
    cq_->Shutdown();
}

void Testgen::runServer(const ProgramInfo *programInfo) {
    std::string server_address("0.0.0.0:50051");
    //P4FuzzGuideImpl service = P4FuzzGuideImpl(programInfo);
    P4FuzzGuide::AsyncService service_;
    std::map<std::string, ConcolicExecutor*> coverageMap;
    const ProgramInfo* programInfo_;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    cq_ = builder.AddCompletionQueue();
    server_ = builder.BuildAndStart();

    //std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    //server->Wait();

    new GetP4StatementData(&service_, cq_.get(), programInfo);
    new GetP4CoverageData(&service_, cq_.get(), programInfo);
    new RecordP4TestgenData(&service_, cq_.get(), programInfo);

    void *tag;
    bool ok;
    while (true) {
        GPR_ASSERT(cq_->Next(&tag, &ok));
        GPR_ASSERT(ok);
        static_cast<CallData*>(tag)->Proceed(coverageMap);
    }
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

int Testgen::mainImpl(const IR::P4Program *program) {
    // Get the options and the seed.
    const auto &testgenOptions = TestgenOptions::get();

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

    // Run interactive mode
    if (testgenOptions.interactive) {
        runServer(programInfo);
        return EXIT_SUCCESS;
    }

    auto seed = Utils::getCurrentSeed();
    if (seed) {
        printFeature("test_info", 4, "============ Program seed %1% =============\n", *seed);
    }

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

    if (testgenOptions.pathSelectionPolicy ==
            PathSelectionPolicy::TestCase) {
        auto *concExec = new ConcolicExecutor(*programInfo);
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

    } else {
        // Need to declare the solver here to ensure its lifetime.
        Z3Solver solver;

        auto *symExec = pickExecutionEngine(testgenOptions, programInfo, solver);

        // Define how to handle the final state for each test. This is target defined.
        auto *testBackend = TestgenTarget::getTestBackend(*programInfo, *symExec, testPath, seed);
        // Each test back end has a different run function.
        // We delegate execution to the symbolic executor.
        auto callBack = [testBackend](auto &&finalState) {
            return testBackend->run(std::forward<decltype(finalState)>(finalState));
        };

        try {
            // Run the symbolic executor with given exploration strategy.
            symExec->run(callBack);
        } catch (...) {
            if (testgenOptions.trackBranches) {
                // Print list of the selected branches and store all information into
                // dumpFolder/selectedBranches.txt file.
                // This printed list could be used for repeat this bug in arguments of --input-branches
                // command line. For example, --input-branches "1,1".
                symExec->printCurrentTraceAndBranches(std::cerr);
            }
            throw;
        }
        // Emit a performance report, if desired.
        testBackend->printPerformanceReport(true);

        // Do not print this warning if assertion mode is enabled.
        if (testBackend->getTestCount() == 0 && !testgenOptions.assertionModeEnabled) {
            ::warning(
                    "Unable to generate tests with given inputs. Double-check provided options and "
                    "parameters.\n");
        }
    }

    return ::errorCount() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace P4Tools::P4Testgen
