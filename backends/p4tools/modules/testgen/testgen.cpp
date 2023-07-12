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
    if (server_ != nullptr)
        server_->Shutdown();
    if (cq_ != nullptr)
        cq_->Shutdown();
}

void Testgen::runServer(const ProgramInfo *programInfo, int grpcPort) {
    std::string server_address("0.0.0.0:");
    server_address += std::to_string(grpcPort);
    //P4FuzzGuideImpl service = P4FuzzGuideImpl(programInfo);
    P4FuzzGuide::AsyncService service_;
    std::map<std::string, ConcolicExecutor*> coverageMap;

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

    CallData::CallStatus callStatus;
    void *tag;
    bool ok, callAgain = false;
    std::string devId;
    TestCase testCase;      // Mutable
    while (true) {
        callStatus = CallData::CREATE;

        GPR_ASSERT(cq_->Next(&tag, &ok));
        GPR_ASSERT(ok);
        do {
            callStatus = static_cast<CallData*>(tag)->Proceed(coverageMap, devId, testCase, callStatus);

            if (callStatus == CallData::REQ) {
                std::cout << "Record P4 Coverage of device: " << devId << std::endl;

                if (coverageMap.count(devId) == 0) {
                    coverageMap.insert(std::make_pair(devId,
                                new ConcolicExecutor(*programInfo)));
                }

                auto* stateMgr = coverageMap.at(devId);
                callStatus = CallData::ERROR;

                try {
                    stateMgr->run(testCase);
                    callStatus = CallData::RET;

                } catch (const Util::CompilerBug &e) {
                    std::cerr << "Internal compiler error: " << e.what() << std::endl;
                    std::cerr << "Please submit a bug report with your code." << std::endl;

                } catch (const Util::CompilationError &e) {
                    std::cerr << "Compilation error: " << e.what() << std::endl;

                } catch (const std::exception &e) {
                    std::cerr << "Internal error: " << e.what() << std::endl;
                    std::cerr << "Please submit a bug report with your code." << std::endl;
                }
                callAgain = true;

            } else {
                callAgain = false;
            }
        } while (callAgain);
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

    try {
        // Run the symbolic executor with given exploration strategy.
        symbex.run(callBack);
    } catch (...) {
        if (testgenOptions.trackBranches) {
            // Print list of the selected branches and store all information into
            // dumpFolder/selectedBranches.txt file.
            // This printed list could be used for repeat this bug in arguments of --input-branches
            // command line. For example, --input-branches "1,1".
            symbex.printCurrentTraceAndBranches(std::cerr);
        }
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

    if (testgenOptions.interactive) {
        runServer(programInfo, testgenOptions.grpcPort);
        return EXIT_SUCCESS;
    }

    if (testgenOptions.pathSelectionPolicy == PathSelectionPolicy::TestCase) {
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
        return EXIT_SUCCESS;
    }

    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbex = pickExecutionEngine(testgenOptions, programInfo, solver);

    return generateAbstractTests(testgenOptions, programInfo, *symbex);
}

}  // namespace P4Tools::P4Testgen
