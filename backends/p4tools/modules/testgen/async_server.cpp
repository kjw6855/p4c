#include "backends/p4tools/modules/testgen/async_server.h"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/testgen/lib/test_backend.h"

namespace P4Tools::P4Testgen {

P4FuzzGuideImpl::P4FuzzGuideImpl(const ProgramInfo *programInfo)
: programInfo_(programInfo) {}

static std::string hexToByteString(const std::string &hex) {
    char *bytes = (char*)std::malloc(hex.length() / 2);

    for (unsigned int i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes[i/2] = (char) strtol(byteString.c_str(), NULL, 16);
    }

    auto retStr = std::string(reinterpret_cast<char*>(bytes), hex.length()/2);
    free(bytes);

    return retStr;
}

Status P4FuzzGuideImpl::GetP4Statement(ServerContext* context,
        const P4StatementRequest* req,
        P4StatementReply* rep) {

    auto allNodes = programInfo_->getCoverableNodes();

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

Status P4FuzzGuideImpl::GetP4Coverage(ServerContext* context,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();

    auto allNodes = programInfo_->getCoverableNodes();
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

Status P4FuzzGuideImpl::RecordP4Testgen(ServerContext* context,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();
    std::cout << "Record P4 Coverage of device: " << devId << std::endl;

    auto allNodes = programInfo_->getCoverableNodes();
    if (coverage_map.count(devId) == 0) {
        coverage_map.insert(std::make_pair(devId,
                    new ConcolicExecutor(*programInfo_)));
    }

    auto* stateMgr = coverage_map.at(devId);

    try {
        stateMgr->run(req->test_case());
#if 0
        auto testPath = std::filesystem::path("/tmp/p4testgen_out/tmp").stem();
        auto *testBackend = TestgenTarget::getTestBackend(*programInfo_, *stateMgr, testPath, Utils::getCurrentSeed());
        auto callBack = [testBackend](auto &&finalState) {
            return testBackend->run(std::forward<decltype(finalState)>(finalState));
        };
        stateMgr->run(callBack);
#endif

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

void GetP4StatementData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap) {
    switch (status_) {
        case CallData::PROCESS:
        {
            new GetP4StatementData(service_, cq_, programInfo_);

            auto allNodes = programInfo_->getCoverableNodes();

            int i = 1, idx = request_.idx();
            for (auto node : allNodes) {
                if (i++ != idx)
                    continue;

                std::stringstream ss;
                ss << node;
                reply_.set_statement(ss.str());
                break;
            }

            status_ = CallData::FINISH;
            responder_.Finish(reply_, Status::OK, this);
            break;
        }

        case CallData::FINISH:
            delete this;
            break;
    }
}

void GetP4CoverageData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap) {
    switch (status_) {
        case CallData::PROCESS:
        {
            new GetP4CoverageData(service_, cq_, programInfo_);

            auto devId = request_.device_id();
            auto allNodes = programInfo_->getCoverableNodes();
            std::cout << "Get P4 Coverage of device: " << devId << std::endl;

            if (coverageMap.count(devId) == 0) {
                reply_.set_stmt_cov_bitmap("");
                reply_.set_stmt_cov_size(allNodes.size());
                reply_.set_action_cov_bitmap("");
                reply_.set_action_cov_size(0);
            } else {
                auto* stateMgr = coverageMap.at(devId);
                reply_.set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
                reply_.set_stmt_cov_size(stateMgr->statementBitmapSize);
                reply_.set_action_cov_bitmap("");
                reply_.set_action_cov_size(1);
            }
            status_ = CallData::FINISH;
            responder_.Finish(reply_, Status::OK, this);
            break;
        }

        case CallData::FINISH:
            delete this;
            break;
    }
}

void RecordP4TestgenData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap) {
    switch (status_) {
        case CallData::PROCESS:
        {
            new RecordP4TestgenData(service_, cq_, programInfo_);

            auto devId = request_.device_id();
            auto allNodes = programInfo_->getCoverableNodes();
            std::cout << "Record P4 Coverage of device: " << devId << std::endl;

            if (coverageMap.count(devId) == 0) {
                coverageMap.insert(std::make_pair(devId,
                            new ConcolicExecutor(*programInfo_)));
            }

            auto* stateMgr = coverageMap.at(devId);
            bool isSuc = false;

            try {
                stateMgr->run(request_.test_case());
                isSuc = true;

            } catch (const Util::CompilerBug &e) {
                std::cerr << "Internal compiler error: " << e.what() << std::endl;
                std::cerr << "Please submit a bug report with your code." << std::endl;

            } catch (const Util::CompilationError &e) {
                std::cerr << "Compilation error: " << e.what() << std::endl;

            } catch (const std::exception &e) {
                std::cerr << "Internal error: " << e.what() << std::endl;
                std::cerr << "Please submit a bug report with your code." << std::endl;
            }

            if (isSuc) {
                auto* newTestCase = new TestCase(request_.test_case());
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

                reply_.set_allocated_test_case(newTestCase);
                reply_.set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
                reply_.set_stmt_cov_size(stateMgr->statementBitmapSize);
                reply_.set_action_cov_bitmap("");
                reply_.set_action_cov_size(1);
            }

            status_ = CallData::FINISH;
            responder_.Finish(reply_, isSuc ? Status::OK : Status::CANCELLED, this);
            break;
        }

        case CallData::FINISH:
            delete this;
            break;
    }
}

} // namespace P4Tools::P4Testgen
