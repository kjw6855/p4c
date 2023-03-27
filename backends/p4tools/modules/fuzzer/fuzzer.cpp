#include "backends/p4tools/modules/fuzzer/fuzzer.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/util.h"
#include "frontends/common/parser_options.h"
#include "lib/cstring.h"
#include "lib/error.h"

#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/exploration_strategy.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/inc_max_coverage_stack.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/incremental_stack.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/linear_enumeration.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/random_access_stack.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/rnd_access_max_coverage.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/selected_branches.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/selected_test.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/target.h"
#include "backends/p4tools/modules/fuzzer/lib/logging.h"
#include "backends/p4tools/modules/fuzzer/lib/test_backend.h"
#include "backends/p4tools/modules/fuzzer/lib/continuation.h"
#include "backends/p4tools/modules/fuzzer/options.h"
#include "backends/p4tools/modules/fuzzer/register.h"

#include "backends/p4tools/modules/fuzzer/p4fuzzer.grpc.pb.h"

namespace fs = boost::filesystem;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using p4fuzzer::P4FuzzGuide;
using p4fuzzer::P4CoverageRequest;
using p4fuzzer::P4CoverageReply;
using p4testgen::TestCase;

namespace P4Tools {

namespace P4Testgen {

class P4FuzzGuideImpl final : public P4FuzzGuide::Service {
    private:
        std::map<std::string, ExplorationStrategy*> coverage_map_;
        AbstractSolver* solver_;
        const ProgramInfo* programInfo_;
        const boost::filesystem::path* testPath_;
        boost::optional<uint32_t> seed_;
        template<typename Base, typename T>
        inline bool instanceof(const T *ptr) {
            return dynamic_cast<const Base*>(ptr) != nullptr;
        }

        void recordTestCase(ExecutionState* estate, const TestCase testCase) {

            std::cout << "[ENTITY]" << std::endl;
            for (const ::p4::v1::Entity &entity : testCase.entities()) {
                // TODO: find IR::Statement from Entity
                if (!entity.has_table_entry())
                    continue;

                auto entry = entity.table_entry();

                // Matches
                for  (const ::p4::v1::FieldMatch &match : entry.match()) {
                    // TODO
                }

                // Actions
                auto action = entry.action();
                std::cout << "[" << entry.table_id() << "] "
                    << entry.match_size() << " matches and action: "
                    << action.action().action_id() << std::endl;

                //estate->markVisited(stmt);
            }

            // IR::P4Program
            auto cmds = {programInfo_->program};
            std::cout << std::endl << "[PROGRAM INFO]" << std::endl;
            int node_cnt = 0;
            for (auto it = cmds.begin(); it != cmds.end(); ++it) {
                auto node = *it;
                if (instanceof<IR::Node>(node)) {
                    node_cnt ++;
                }
            }
            std::cout << "# nodes: "
                << node_cnt << "/"
                << cmds.size() << std::endl;

            // step
            auto* stepper = TestgenTarget::getCmdStepper(*estate, *solver_, *programInfo_);
            auto* result = stepper->step(programInfo_->program);
        }

    public:
        P4FuzzGuideImpl(AbstractSolver *solver,
                const ProgramInfo* programInfo,
                const boost::filesystem::path* testPath,
                boost::optional<uint32_t> seed) {
            solver_ = solver;
            programInfo_ = programInfo;
            testPath_ = testPath;
            seed_ = seed;
        }

        bool Callback_run(const FinalState& state) {
            //const auto* execState = state.getExecutionState();
            std::cout << std::endl << "[FINAL]" << std::endl;
            for (auto* visited : state.getVisited()) {
                std::cout << "[" << visited->node_type_name()
                   << "] " << visited << std::endl;
            }
            return true;
        }

        Status GetP4Coverage(ServerContext* context,
                const P4CoverageRequest* req,
                P4CoverageReply* rep) override {

            auto devId = req->device_id();

            auto allStmts = programInfo_->getAllStatements();
            std::cout << "Get P4 Coverage of device: " << devId << std::endl;
            for (auto stmt : allStmts) {
                /*
                std::stringstream ss;
                JSONGenerator jsonGen(ss);
                //jsonGen << stmt;
                stmt->toJSON(jsonGen);
                std::cout << ss.str() << std::endl;
                */
            }

            if (coverage_map_.count(devId) == 0) {
                rep->set_hit_stmt_count(0);
                rep->set_total_stmt_count(allStmts.size());
                rep->set_hit_action_count(0);
                rep->set_total_action_count(0);
            } else {
                auto* symExec = coverage_map_.at(devId);
                rep->set_hit_stmt_count(symExec->getVisitedStatements().size());
                rep->set_total_stmt_count(allStmts.size());
                rep->set_hit_action_count(1);
                rep->set_total_action_count(1);
            }
            return Status::OK;
        }

        Status RecordP4Testgen(ServerContext* context,
                const P4CoverageRequest* req,
                P4CoverageReply* rep) override {

            auto devId = req->device_id();
            std::cout << "Record P4 Coverage of device: " << devId << std::endl;

            auto allStmts = programInfo_->getAllStatements();

            if (coverage_map_.count(devId) == 0) {
                coverage_map_.insert(std::make_pair(devId,
                            new SelectedTest(*solver_, *programInfo_, seed_)));

                            //new ExecutionState(programInfo_->program)));
            }

            auto* symExec = coverage_map_.at(devId);
            ExplorationStrategy::Callback callBack =
                std::bind(&P4FuzzGuideImpl::Callback_run, this, std::placeholders::_1);

            // TODO: run once again
            symExec->run(callBack);
            //recordTestCase(estate, req->test_case());
            //rep->set_hit_stmt_count(estate->getVisited().size());
            rep->set_hit_stmt_count(symExec->getVisitedStatements().size());
            rep->set_total_stmt_count(allStmts.size());
            rep->set_hit_action_count(1);
            rep->set_total_action_count(1);

            return Status::OK;
        }
};

void Testgen::registerTarget() {
    // Register all available compiler targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerCompilerTargets();
}

void runServer(AbstractSolver* solver,
        const ProgramInfo* programInfo,
        const boost::filesystem::path* testPath,
        boost::optional<uint32_t> seed) {
    std::string server_address("0.0.0.0:50051");
    P4FuzzGuideImpl service = P4FuzzGuideImpl(solver, programInfo, testPath, seed);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}

int Testgen::mainImpl(const IR::P4Program* program) {
    // Register all available fuzzer targets.
    // These are discovered by CMAKE, which fills out the register.h.in file.
    registerFuzzerTargets();

    const auto* programInfo = TestgenTarget::initProgram(program);
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

    auto const inputFile = P4CContext::get().options().file;
    cstring testDirStr = TestgenOptions::get().outputDir;
    auto seed = TestgenOptions::get().seed;

    // Get the basename of the input file and remove the extension
    // This assumes that inputFile is not null.
    auto programName = fs::path(inputFile).filename().replace_extension("");
    // Create the directory, if the directory string is valid and if it does not exist.
    auto testPath = programName;
    if (!testDirStr) {
        testPath.clear();
    } else if (!testDirStr.isNullOrEmpty()) {
        auto testDir = fs::path(testDirStr);
        fs::create_directories(testDir);
        testPath = fs::path(testDir) / testPath;
    }

    if (seed != boost::none) {
        // Initialize the global seed for randomness.
        Utils::setRandomSeed(*seed);
        printFeature("test_info", 4, "============ Program seed %1% =============\n", *seed);
    }

    Z3Solver solver;

    auto symExec = [&solver, &programInfo, seed]() -> ExplorationStrategy* {
        std::string explorationStrategy = TestgenOptions::get().explorationStrategy;
        if (explorationStrategy.compare("randomAccessStack") == 0) {
            // If the user mistakenly specifies an invalid popLevel, we set it to 3.
            int popLevel = TestgenOptions::get().popLevel;
            if (popLevel <= 1) {
                ::warning("--pop-level must be greater than 1; using default value of 3.\n");
                popLevel = 3;
            }
            return new RandomAccessStack(solver, *programInfo, seed, popLevel);
        }
        if (explorationStrategy.compare("linearEnumeration") == 0) {
            // If the user mistakenly specifies an invalid bound, we set it to 2
            // to generate at least 2 tests.
            int linearBound = TestgenOptions::get().linearEnumeration;
            if (linearBound <= 1) {
                ::warning(
                    "--linear-enumeration must be greater than 1; using default value of 2.\n");
                linearBound = 2;
            }
            return new LinearEnumeration(solver, *programInfo, seed, linearBound);
        }
        if (explorationStrategy.compare("maxCoverage") == 0) {
            return new IncrementalMaxCoverageStack(solver, *programInfo, seed);
        }
        if (explorationStrategy.compare("randomAccessMaxCoverage") == 0) {
            // If the user mistakenly sets an invalid saddlePoint, we set it to 5.
            int saddlePoint = TestgenOptions::get().saddlePoint;
            if (saddlePoint <= 1) {
                ::warning("--saddle-point must be greater than 1; using default value of 5.\n");
                saddlePoint = 5;
            }
            return new RandomAccessMaxCoverage(solver, *programInfo, seed, saddlePoint);
        }
        if (!TestgenOptions::get().selectedBranches.empty()) {
            std::string selectedBranchesStr = TestgenOptions::get().selectedBranches;
            return new SelectedBranches(solver, *programInfo, seed, selectedBranchesStr);
        }
        return new IncrementalStack(solver, *programInfo, seed);
    }();

    if (TestgenOptions::get().interactive) {
        // Receive p4testgen.proto message
        // Measure p4 coverage
        runServer(&solver, programInfo, &testPath, seed);

    } else {

        // Define how to handle the final state for each test. This is target defined.
        auto* testBackend = TestgenTarget::getTestBackend(*programInfo, *symExec, testPath, seed);
        ExplorationStrategy::Callback callBack =
            std::bind(&TestBackEnd::run, testBackend, std::placeholders::_1);

        try {
            // Run the symbolic executor with given exploration strategy.
            symExec->run(callBack);
        } catch (...) {
            if (TestgenOptions::get().trackBranches) {
                // Print list of the selected branches and store all information into
                // dumpFolder/selectedBranches.txt file.
                // This printed list could be used for repeat this bug in arguments of --input-branches
                // command line. For example, --input-branches "1,1".
                symExec->printCurrentTraceAndBranches(std::cerr);
            }
            throw;
        }

        if (testBackend->getTestCount() == 0) {
            ::warning(
                    "Unable to generate tests with given inputs. Double-check provided options and "
                    "parameters.\n");
        }
    }

    return ::errorCount() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace P4Testgen

}  // namespace P4Tools
