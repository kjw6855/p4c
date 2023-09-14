#include "backends/p4tools/modules/testgen/async_server.h"

#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "lib/gc.h"

#include "backends/p4tools/common/lib/util.h"
#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/testgen/lib/test_backend.h"

namespace P4Tools::P4Testgen {

P4FuzzGuideImpl::P4FuzzGuideImpl(const ProgramInfo *programInfo)
: programInfo_(programInfo) {}

static std::string hexToByteString(const std::string &hex) {
    char *bytes = (char*)malloc(hex.length() / 2);

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

    auto* newTestCase = new TestCase(req->test_case());
    std::string stmtBitmap, actionBitmap;
    int stmtBitmapSize, actionBitmapSize;
    if (coverageMap.count(devId) == 0) {
        stmtBitmap = "";
        stmtBitmapSize = allNodes.size();
        actionBitmap = "";
        actionBitmapSize = 0;
    } else {
        auto* stateMgr = coverageMap.at(devId);
        stmtBitmap = stateMgr->getStatementBitmapStr();
        stmtBitmapSize = stateMgr->statementBitmapSize;
        actionBitmap = "";
        actionBitmapSize = 1;
    }

    newTestCase->set_stmt_cov_bitmap(stmtBitmap);
    newTestCase->set_stmt_cov_size(stmtBitmapSize);
    newTestCase->set_action_cov_bitmap(actionBitmap);
    newTestCase->set_action_cov_size(actionBitmapSize);

    rep->set_allocated_test_case(newTestCase);

    return Status::OK;
}

Status P4FuzzGuideImpl::RecordP4Testgen(ServerContext* context,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();
    std::cout << "Record P4 Coverage of device: " << devId << std::endl;

    auto allNodes = programInfo_->getCoverableNodes();
    if (coverageMap.count(devId) == 0) {
        coverageMap.insert(std::make_pair(devId,
                    new ConcolicExecutor(*programInfo_)));
    }

    auto *stateMgr = coverageMap.at(devId);
    auto *newTestCase = new TestCase(req->test_case());
    for (auto &entity : *newTestCase->mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        entity.mutable_table_entry()->set_is_valid_entry(0);
    }

    try {
        stateMgr->run(*newTestCase);
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

    newTestCase->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
    newTestCase->set_stmt_cov_size(stateMgr->statementBitmapSize);
    newTestCase->set_action_cov_bitmap("");
    newTestCase->set_action_cov_size(1);

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

    return Status::OK;
}

CallData::CallStatus HelloData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) {
    switch (status_) {
        case CallData::CREATE:
        {
            new HelloData(service_, cq_);
            status_ = CallData::FINISH;
            reply_.set_status(1);
            responder_.Finish(reply_, Status::OK, this);
            break;
        }
        case CallData::FINISH:
            delete this;
            break;
    }

    return status_;
}

CallData::CallStatus GetP4StatementData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase &testCase, CallData::CallStatus callStatus) {
    switch (status_) {
        case CallData::CREATE:
        {
            new GetP4StatementData(service_, cq_, programInfo_);

            auto &allNodes = programInfo_->getCoverableNodes();

            int i = 1, idx = request_.idx();
            for (const auto *node : allNodes) {
                if (i++ != idx)
                    continue;

                const auto &srcInfo = node->getSourceInfo();
                auto sourceLine = srcInfo.toPosition().sourceLine;
                std::stringstream ss;
                ss << srcInfo.getSourceFile() << "\\" << sourceLine << ": " << *node;
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

    return status_;
}

CallData::CallStatus GetP4CoverageData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase &testCase, CallData::CallStatus callStatus) {
    switch (status_) {
        case CallData::CREATE:
        {
            new GetP4CoverageData(service_, cq_, programInfo_);

            auto devId = request_.device_id();
            auto allNodes = programInfo_->getCoverableNodes();
            std::cout << "Get P4 Coverage of device: " << devId << std::endl;

            auto* newTestCase = new TestCase(request_.test_case());
            std::string stmtBitmap, actionBitmap;
            int stmtBitmapSize, actionBitmapSize;
            if (coverageMap.count(devId) == 0) {
                stmtBitmap = "";
                stmtBitmapSize = allNodes.size();
                actionBitmap = "";
                actionBitmapSize = 0;
            } else {
                auto* stateMgr = coverageMap.at(devId);
                stmtBitmap = stateMgr->getStatementBitmapStr();
                stmtBitmapSize = stateMgr->statementBitmapSize;
                actionBitmap = "";
                actionBitmapSize = 1;
            }

            newTestCase->set_stmt_cov_bitmap(stmtBitmap);
            newTestCase->set_stmt_cov_size(stmtBitmapSize);
            newTestCase->set_action_cov_bitmap(actionBitmap);
            newTestCase->set_action_cov_size(actionBitmapSize);

            reply_.set_allocated_test_case(newTestCase);
            status_ = CallData::FINISH;
            responder_.Finish(reply_, Status::OK, this);
            break;
        }

        case CallData::FINISH:
            delete this;
            break;
    }

    return status_;
}

CallData::CallStatus RecordP4TestgenData::Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase &testCase, CallData::CallStatus callStatus) {

    if (callStatus == CallData::ERROR || callStatus == CallData::RET)
        status_ = callStatus;

    switch (status_) {
        case CallData::CREATE:
        {
            new RecordP4TestgenData(service_, cq_, programInfo_);

            devId = request_.device_id();
            status_ = CallData::REQ;
            testCase = request_.test_case();
            for (auto &entity : *testCase.mutable_entities()) {
                if (!entity.has_table_entry())
                    continue;

                entity.mutable_table_entry()->set_is_valid_entry(0);
            }
            break;
        }
        case CallData::REQ:
            break;

        case CallData::ERROR:
        {
            status_ = CallData::FINISH;
            responder_.Finish(reply_, Status::CANCELLED, this);
            break;
        }

        case CallData::RET:
        {
            auto* stateMgr = coverageMap.at(devId);
            auto* newTestCase = new TestCase(testCase);
            newTestCase->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
            newTestCase->set_stmt_cov_size(stateMgr->statementBitmapSize);
            newTestCase->set_action_cov_bitmap("");
            newTestCase->set_action_cov_size(1);

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
            status_ = CallData::FINISH;
            responder_.Finish(reply_, Status::OK, this);
            break;
        }

#if 0
                Z3Solver solver;
                auto *stateMgr = new DepthFirstSearch(solver, *programInfo_);
                auto testPath = std::filesystem::path("/tmp/p4testgen_out/tmp").stem();
                auto *testBackend = TestgenTarget::getTestBackend(*programInfo_, *stateMgr, testPath, Utils::getCurrentSeed());
                auto callBack = [testBackend](auto &&finalState) {
                    return testBackend->run(std::forward<decltype(finalState)>(finalState));
                };
                stateMgr->run(callBack);
#endif

        case CallData::FINISH:
            delete this;
            break;
    }

    return status_;
}

} // namespace P4Tools::P4Testgen
