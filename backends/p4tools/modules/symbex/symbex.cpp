#include "backends/p4tools/modules/symbex/symbex.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
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

#include "backends/p4tools/common/compiler/compiler_target.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "frontends/common/parser_options.h"
#include "frontends/p4/evaluator/evaluator.h"
#include "lib/gc.h"
#include "lib/cstring.h"
#include "lib/error.h"

#include "backends/p4tools/modules/symbex/core/compiler_result.h"
#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/greedy_node_cov.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/random_backtrack.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/selected_branches.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/core/concolic_executor/concolic_executor.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/test_backend.h"
#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/async_server.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/register.h"
#include "backends/p4tools/modules/symbex/toolname.h"
#include "backends/p4tools/modules/symbex/p4symbex.pb.h"

namespace P4::P4Tools::Symbex {

using grpc::ServerBuilder;
using symbex::P4FuzzGuide;
using symbex::TestCase;

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
SymbolicExecutor *pickExecutionEngine(const SymbexOptions &symbexOptions,
                                      const ProgramInfo &programInfo, AbstractSolver &solver) {
    const auto &pathSelectionPolicy = symbexOptions.pathSelectionPolicy;
    if (pathSelectionPolicy == PathSelectionPolicy::GreedyStmtCoverage) {
        return new GreedyNodeSelection(solver, programInfo);
    }
    if (pathSelectionPolicy == PathSelectionPolicy::RandomBacktrack) {
        return new RandomBacktrack(solver, programInfo);
    }
    if (!symbexOptions.selectedBranches.empty()) {
        std::string selectedBranchesStr = symbexOptions.selectedBranches;
        return new SelectedBranches(solver, programInfo, selectedBranchesStr);
    }
    if (pathSelectionPolicy == PathSelectionPolicy::StateDependencyTampering ||
        pathSelectionPolicy == PathSelectionPolicy::StateDependencyTamperingCond ||
        pathSelectionPolicy == PathSelectionPolicy::StateDependencyAlteringPath) {
        const auto *stateDep = programInfo.getCompilerResult().getStateDep();
        if (stateDep == nullptr) {
            error(
                "State-dependency path selection policies require --state-dependency. "
                "Falling back to depth-first search.");
            return new DepthFirstSearch(solver, programInfo);
        }
        StateDependencyPolicy sdPolicy = StateDependencyPolicy::AlteringPath;
        if (pathSelectionPolicy == PathSelectionPolicy::StateDependencyTampering) {
            sdPolicy = StateDependencyPolicy::Tampering;
        } else if (pathSelectionPolicy == PathSelectionPolicy::StateDependencyTamperingCond) {
            sdPolicy = StateDependencyPolicy::TamperingCond;
        }
        return new StateDependencyTracker(solver, programInfo, *stateDep, sdPolicy);
    }
    return new DepthFirstSearch(solver, programInfo);
}

/// Analyse the results of the symbolic execution and generate diagnostic messages.
int postProcess(const SymbexOptions &symbexOptions, const TestBackEnd &testBackend) {
    // Do not print this warning if assertion mode is enabled.
    if (testBackend.getTestCount() == 0 && !symbexOptions.assertionModeEnabled) {
        warning(
            "Unable to generate tests with given inputs. Double-check provided options and "
            "parameters.\n");
    }
    if (testBackend.getCoverage() < symbexOptions.minCoverage) {
        error("The tests did not achieve requested coverage of %1%, the coverage is %2%.",
              symbexOptions.minCoverage, testBackend.getCoverage());
    }

    return errorCount() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

std::optional<AbstractTestList> generateAndCollectAbstractTests(
    const SymbexOptions &symbexOptions, const ProgramInfo &programInfo) {
    if (!symbexOptions.testBaseName.has_value()) {
        error(
            "Test collection requires a test name. No name was provided as part of the "
            "Symbex options.");
        return std::nullopt;
    }

    // The test name is the stem of the output base path.
    TestBackendConfiguration testBackendConfiguration{symbexOptions.testBaseName.value(),
                                                      symbexOptions.maxTests, std::nullopt,
                                                      symbexOptions.seed};
    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbolicExecutor = pickExecutionEngine(symbexOptions, programInfo, solver);

    // Each test back end has a different run function.
    auto *testBackend =
        SymbexTarget::getTestBackend(programInfo, testBackendConfiguration, *symbolicExecutor);

    // For the Tampering policy, use the three-phase entry point so that the test backend
    // receives all three related packet pairs as a unit.
    auto *tracker = dynamic_cast<StateDependencyTracker *>(symbolicExecutor);
    if (tracker != nullptr &&
        (tracker->getPolicy() == StateDependencyPolicy::Tampering ||
         tracker->getPolicy() == StateDependencyPolicy::TamperingCond)) {
        tracker->runTampering([testBackend](const TamperingFinalState &ts) {
            return testBackend->runTampering(ts);
        });
    } else {
        symbolicExecutor->run([testBackend](auto &&finalState) {
            return testBackend->run(std::forward<decltype(finalState)>(finalState));
        });
    }
    auto result = postProcess(symbexOptions, *testBackend);
    if (result != EXIT_SUCCESS) {
        return std::nullopt;
    }
    return testBackend->getTests();
}

int generateAndWriteAbstractTests(const SymbexOptions &symbexOptions,
                                  const ProgramInfo &programInfo) {
    std::filesystem::path testPath;
    /// If the test name is not provided, use the steam of the input file name as test name.
    if (symbexOptions.testBaseName.has_value()) {
        testPath = symbexOptions.testBaseName.value().c_str();
    } else if (!symbexOptions.file.empty()) {
        testPath = symbexOptions.file.stem();
    } else {
        error("Neither a file nor test base name was set. Can not infer a test name.");
    }

    // Create the directory, if the directory string is valid and if it does not exist.
    if (symbexOptions.outputDir.has_value()) {
        auto testDir = symbexOptions.outputDir.value();
        try {
            std::filesystem::create_directories(testDir);
        } catch (const std::exception &err) {
            error("Unable to create directory %1%: %2%", testDir.c_str(), err.what());
            return EXIT_FAILURE;
        }
        testPath = testDir / testPath;
    }

    // The test name is the stem of the output base path.
    TestBackendConfiguration testBackendConfiguration{
        cstring(testPath.c_str()), symbexOptions.maxTests, testPath, symbexOptions.seed};

    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;
    auto *symbolicExecutor = pickExecutionEngine(symbexOptions, programInfo, solver);

    // Each test back end has a different run function.
    auto *testBackend =
        SymbexTarget::getTestBackend(programInfo, testBackendConfiguration, *symbolicExecutor);

    // For the Tampering policy, use the three-phase entry point so that the test backend
    // receives all three related packet pairs as a unit.
    auto *tracker = dynamic_cast<StateDependencyTracker *>(symbolicExecutor);
    if (tracker != nullptr &&
        (tracker->getPolicy() == StateDependencyPolicy::Tampering ||
         tracker->getPolicy() == StateDependencyPolicy::TamperingCond)) {
        tracker->runTampering([testBackend](const TamperingFinalState &ts) {
            return testBackend->runTampering(ts);
        });
    } else {
        symbolicExecutor->run([testBackend](auto &&finalState) {
            return testBackend->run(std::forward<decltype(finalState)>(finalState));
        });
    }
    return postProcess(symbexOptions, *testBackend);
}

std::optional<AbstractTestList> generateTestsImpl(std::optional<std::string_view> program,
                                                  const SymbexOptions &symbexOptions,
                                                  bool writeTests) {
    P4Tools::Target::init(symbexOptions.target.c_str(), symbexOptions.arch.c_str());

    CompilerResultOrError compilerResultOpt;
    if (program.has_value()) {
        // Run the compiler to get an IR and invoke the tool.
        compilerResultOpt = P4Tools::CompilerTarget::runCompiler(symbexOptions, TOOL_NAME,
                                                                 std::string(program.value()));
    } else {
        if (symbexOptions.file.empty()) {
            error("Expected a file input.");
            return std::nullopt;
        }
        // Run the compiler to get an IR and invoke the tool.
        compilerResultOpt = P4Tools::CompilerTarget::runCompiler(symbexOptions, TOOL_NAME);
    }

    if (!compilerResultOpt.has_value()) {
        error("Failed to run the compiler.");
        return std::nullopt;
    }

    const auto *symbexCompilerResult =
        compilerResultOpt.value().get().checkedTo<SymbexCompilerResult>();

    if (symbexOptions.stateDep) {
        const auto *program = &compilerResultOpt.value().get().getProgram();
        bool isv1 = symbexOptions.langVersion == CompilerOptions::FrontendVersion::P4_14;
        // Only compute the chain categories the active policy actually consumes: the Tampering
        // tracker reads only dataWriteKeyChains (SD_KEY), AlteringPath only dataWriteCondChains
        // (SD_COND). Skipping the other IFDS passes (A2S2V, H2S2V, and the unused sink) is the
        // dominant analysis-time saving for these runs. Other policies keep the full analysis.
        unsigned sdCats = P4StateDependency::SD_ALL;
        if (symbexOptions.pathSelectionPolicy == PathSelectionPolicy::StateDependencyTampering)
            sdCats = P4StateDependency::SD_KEY;
        else if (symbexOptions.pathSelectionPolicy ==
                 PathSelectionPolicy::StateDependencyAlteringPath)
            sdCats = P4StateDependency::SD_COND;
        auto *stateDep = new P4StateDependency::StateDependencyResult(
            P4StateDependency::runStateDependencyAnalysis(program, cstring(symbexOptions.arch), isv1,
                                                          {}, sdCats));
        if (::P4::errorCount() > 0) return std::nullopt;
        symbexCompilerResult->setStateDep(stateDep);
    }

    const auto *programInfo = SymbexTarget::produceProgramInfo(*symbexCompilerResult);
    if (programInfo == nullptr || errorCount() > 0) {
        error("Symbex encountered errors during preprocessing.");
        return std::nullopt;
    }

    if (writeTests) {
        int result = generateAndWriteAbstractTests(symbexOptions, *programInfo);
        if (result != EXIT_SUCCESS) {
            return std::nullopt;
        }
        return {};
    }
    return generateAndCollectAbstractTests(symbexOptions, *programInfo);
}

}  // namespace

void Symbex::runServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
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

void Symbex::runAsyncServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
        const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        int grpcPort) {
    std::string server_address("0.0.0.0:");
    server_address += std::to_string(grpcPort);

    ServerState state;
    std::map<std::string, ConcolicExecutor*> coverageMap;

    P4FuzzGuide::AsyncService asyncService;
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&asyncService);
    auto cq = builder.AddCompletionQueue();
    server = builder.BuildAndStart();
    std::cout << "Server listening on " << server_address << std::endl;

    // Seed one handler per RPC type — all will be processed on this (main) thread,
    // avoiding the BDW GC / TLS DTV collection issue that affects the sync thread pool.
    new HelloData(&asyncService, cq.get());
    new GetP4NameData(&asyncService, cq.get(), tableCollector);
    new GetP4CoverageData(&asyncService, cq.get(), *programInfo, tableCollector);
    new GetP4StatementData(&asyncService, cq.get(), *programInfo, tableCollector);
    new RecordSymbexData(&asyncService, cq.get(), *programInfo, tableCollector,
                         top, refMap, typeMap, &state);
    new GenRuleSymbexData(&asyncService, cq.get(), *programInfo, tableCollector,
                          top, refMap, typeMap, &state);

    void *tag;
    bool ok;
    while (cq->Next(&tag, &ok)) {
        std::string devId;
        TestCase testCase;
        static_cast<CallData*>(tag)->Proceed(coverageMap, devId, testCase,
                                              ok ? CallData::REQ : CallData::ERROR);
        {
            std::lock_guard<std::mutex> lock(state.shutdown_mu);
            if (state.shutdown_requested) break;
        }
    }

    server->Shutdown();
    cq->Shutdown();
    // Drain remaining cancellation events before destroying the queue
    while (cq->Next(&tag, &ok)) {}
    server->Wait();
    std::cout << "Shutdown server gracefully" << std::endl;
}

void Symbex::registerTarget() {
    // Register all available Symbex targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerSymbexTargets();
}

int Symbex::mainImpl(const CompilerResult &compilerResult) {
    const auto &symbexOptions = SymbexOptions::get();

    // Type-check early so we can inject stateDep before produceProgramInfo allocates
    // coverage sets — this ensures the IFDS heap (freed inside runStateDependencyAnalysis
    // via GC_gcollect_and_unmap) is returned to the OS before symbex begins.
    const auto *symbexCompilerResult = compilerResult.checkedTo<SymbexCompilerResult>();

    if (symbexOptions.stateDep) {
        const auto *program = &compilerResult.getProgram();
        bool isv1 = symbexOptions.langVersion == CompilerOptions::FrontendVersion::P4_14;
        // Only compute the chain categories the active policy consumes: Tampering reads only
        // dataWriteKeyChains (SD_KEY), AlteringPath only dataWriteCondChains (SD_COND). Skipping
        // the other IFDS passes is the dominant analysis-time saving for these runs.
        unsigned sdCats = P4StateDependency::SD_ALL;
        if (symbexOptions.pathSelectionPolicy == PathSelectionPolicy::StateDependencyTampering)
            sdCats = P4StateDependency::SD_KEY;
        else if (symbexOptions.pathSelectionPolicy ==
                 PathSelectionPolicy::StateDependencyAlteringPath)
            sdCats = P4StateDependency::SD_COND;
        auto *stateDep = new P4StateDependency::StateDependencyResult(
            P4StateDependency::runStateDependencyAnalysis(program, cstring(symbexOptions.arch), isv1,
                                                          {}, sdCats));
        if (::P4::errorCount() > 0) return 1;
        symbexCompilerResult->setStateDep(stateDep);
    }

    const auto *programInfo = SymbexTarget::produceProgramInfo(*symbexCompilerResult);
    if (programInfo == nullptr || errorCount() > 0) {
        error("Symbex encountered errors during preprocessing.");
        return EXIT_FAILURE;
    }

    if (symbexOptions.interactive) {
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

        runAsyncServer(programInfo, tableCollector, top,
                &midEnd.refMap, &midEnd.typeMap, symbexOptions.grpcPort);
        return EXIT_SUCCESS;
    }

    return generateAndWriteAbstractTests(symbexOptions, *programInfo);
}

std::optional<AbstractTestList> Symbex::generateTests(std::string_view program,
                                                       const SymbexOptions &symbexOptions) {
    try {
        return generateTestsImpl(program, symbexOptions, false);
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << "\n";
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<AbstractTestList> Symbex::generateTests(const SymbexOptions &symbexOptions) {
    try {
        return generateTestsImpl(std::nullopt, symbexOptions, false);
    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << "\n";
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

int Symbex::writeTests(std::string_view program, const SymbexOptions &symbexOptions) {
    try {
        if (generateTestsImpl(program, symbexOptions, true).has_value()) {
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

int Symbex::writeTests(const SymbexOptions &symbexOptions) {
    try {
        if (generateTestsImpl(std::nullopt, symbexOptions, true).has_value()) {
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

}  // namespace P4::P4Tools::Symbex
