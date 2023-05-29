#include "backends/p4tools/modules/testgen/testgen.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/common/lib/util.h"
#include "frontends/common/parser_options.h"
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
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/register.h"
#include "backends/p4tools/modules/testgen/p4testgen.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using p4testgen::P4FuzzGuide;
using p4testgen::P4CoverageRequest;
using p4testgen::P4CoverageReply;
using p4testgen::P4StatementRequest;
using p4testgen::P4StatementReply;
using p4testgen::TestCase;

namespace P4Tools::P4Testgen {

class P4FuzzGuideImpl final : public P4FuzzGuide::Service {
    private:
        std::map<std::string, ConcolicExecutor*> coverage_map;
        AbstractSolver &solver;
        const ProgramInfo* programInfo;

        std::string hexToByteString(const std::string& hex) {
            char *bytes = (char*)std::malloc(hex.length() / 2);

            for (unsigned int i = 0; i < hex.length(); i += 2) {
                std::string byteString = hex.substr(i, 2);
                bytes[i/2] = (char) strtol(byteString.c_str(), NULL, 16);
            }

            auto retStr = std::string(reinterpret_cast<char*>(bytes), hex.length()/2);
            free(bytes);

            return retStr;
        }

    public:
        P4FuzzGuideImpl(AbstractSolver &solver, const ProgramInfo* programInfo)
        : solver(solver), programInfo(programInfo) {}

        Status GetP4Statement(ServerContext* context,
                const P4StatementRequest* req,
                P4StatementReply* rep) override {

            auto allNodes = programInfo->getCoverableNodes();

            int i = 1, idx = req->idx();
            for (auto node : allNodes) {
                if (i++ != idx)
                    continue;

                std::stringstream ss;
                ss << node;
                rep->set_statement(ss.str());
                break;
            }

            return Status::OK;
        }

        Status GetP4Coverage(ServerContext* context,
                const P4CoverageRequest* req,
                P4CoverageReply* rep) override {

            auto devId = req->device_id();

            auto allNodes = programInfo->getCoverableNodes();
            std::cout << "Get P4 Coverage of device: " << devId << std::endl;

            if (coverage_map.count(devId) == 0) {
                rep->set_stmt_cov_bitmap("");
                rep->set_stmt_cov_size(allNodes.size());
                rep->set_action_cov_bitmap("");
                rep->set_action_cov_size(0);
            } else {
                auto* stateMgr = coverage_map.at(devId);
                rep->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
                rep->set_stmt_cov_size(stateMgr->statementBitmapSize);
                rep->set_action_cov_bitmap("");
                rep->set_action_cov_size(1);
            }
            return Status::OK;
        }

        Status RecordP4Testgen(ServerContext* context,
                const P4CoverageRequest* req,
                P4CoverageReply* rep) override {

            auto devId = req->device_id();
            std::cout << "Record P4 Coverage of device: " << devId << std::endl;

            auto allNodes = programInfo->getCoverableNodes();
            if (coverage_map.count(devId) == 0) {
                coverage_map.insert(std::make_pair(devId,
                            new ConcolicExecutor(solver, *programInfo)));
            }

            auto* stateMgr = coverage_map.at(devId);

            try {
                stateMgr->run(req->test_case());

            } catch (const Util::CompilerBug &e) {
                std::cerr << "Internal compiler error: " << e.what() << std::endl;
                std::cerr << "Please submit a bug report with your code." << std::endl;
                return Status::CANCELLED;

            } catch (const Util::CompilationError &e) {
                std::cerr << "Compilation error: " << e.what() << std::endl;
                return Status::CANCELLED;

            } catch (const std::exception &e) {
                std::cerr << "Internal error: " << e.what() << std::endl;
                std::cerr << "Please submit a bug report with your code." << std::endl;
                return Status::CANCELLED;
            }

            auto* newTestCase = new TestCase(req->test_case());
            // TODO: multiple output Packets
            auto outputPacketOpt = stateMgr->getOutputPacket();
            newTestCase->clear_expected_output_packet();
            if (outputPacketOpt != boost::none) {
                auto outputPacket = outputPacketOpt.get();
                auto* output = newTestCase->add_expected_output_packet();
                const auto* payload = outputPacket.getEvaluatedPayload();
                const auto* payloadMask = outputPacket.getEvaluatedPayloadMask();

                output->set_port(outputPacket.getPort());
                output->set_packet(hexToByteString(formatHexExpr(payload, false, true, false)));
                output->set_packet_mask(hexToByteString(formatHexExpr(payloadMask, false, true, false)));
            }

            rep->set_allocated_test_case(newTestCase);
            rep->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
            rep->set_stmt_cov_size(stateMgr->statementBitmapSize);
            rep->set_action_cov_bitmap("");
            rep->set_action_cov_size(1);

            return Status::OK;
        }
};

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

void Testgen::runServer(AbstractSolver &solver, const ProgramInfo* programInfo) {
    std::string server_address("0.0.0.0:50051");
    P4FuzzGuideImpl service = P4FuzzGuideImpl(solver, programInfo);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
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

    // Need to declare the solver here to ensure its lifetime.
    Z3Solver solver;

    if (testgenOptions.interactive) {
        runServer(solver, programInfo);

    } else {
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
