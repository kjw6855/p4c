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

static std::string hexToByteString(const std::string &hex) {
    int byteLen = (hex.length() + 1) / 2;

    char *bytes = (char*)malloc(byteLen);

    for (unsigned int i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        bytes[i/2] = (char) strtol(byteString.c_str(), NULL, 16);
    }

    auto retStr = std::string(reinterpret_cast<char*>(bytes), byteLen);
    free(bytes);

    return retStr;
}

/**
 * SYNC API
 */

P4FuzzGuideImpl::P4FuzzGuideImpl(std::map<std::string, ConcolicExecutor*> &coverageMap,
        const ProgramInfo *programInfo, TableCollector &tableCollector,
        const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap)
    : coverageMap(coverageMap),
      programInfo(programInfo),
      tableCollector(tableCollector),
      top(top),
      refMap(refMap),
      typeMap(typeMap) {}

Status P4FuzzGuideImpl::Hello(ServerContext* context,
        const HealthCheckRequest* req,
        HealthCheckResponse* rep) {
    rep->set_status(1);
    return Status::OK;
}

Status P4FuzzGuideImpl::GetP4Name(ServerContext *context,
        const P4NameRequest *req,
        P4NameReply *rep) {

    rep->set_entity_type(req->entity_type());
    switch (req->entity_type()) {
        case p4testgen::TABLE:
            for (const auto *table : tableCollector.getP4TableSet()) {
                rep->add_name(table->controlPlaneName());
            }
            break;

        case p4testgen::ACTION:
            {
                auto p4TableActions = tableCollector.getActions(req->target());
                for (const auto *action : p4TableActions)
                    rep->add_name(action->checkedTo<IR::P4Action>()->controlPlaneName());
                break;
            }

        case p4testgen::MATCH:
            {
                for (const auto *table : tableCollector.getP4TableSet()) {
                    if (table->controlPlaneName() == req->target()) {
                        for (const auto *key : table->getKey()->keyElements) {
                            const IR::Expression* keyExpr = key->expression;
                            const auto* keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
                            rep->add_name(key->getAnnotation("name")->getName());
                            rep->add_type(key->matchType->toString());
                            rep->add_bit_len(keyType->width_bits());
                        }
                        break;
                    }
                }
                break;
            }
        case p4testgen::PARAM:
            {
                for (const auto *action : tableCollector.getActionNodes()) {
                    const auto *p4Action = action->checkedTo<IR::P4Action>();
                    if (p4Action->controlPlaneName() == req->target()) {
                        for (const auto *param : *p4Action->parameters) {
                            const auto* paramType = param->type->checkedTo<IR::Type_Bits>();
                            rep->add_name(param->controlPlaneName());
                            rep->add_bit_len(paramType->width_bits());
                        }
                        break;
                    }
                }
                break;
            }
    }

    return Status::OK;
}

Status P4FuzzGuideImpl::GetP4Statement(ServerContext* context,
        const P4StatementRequest* req,
        P4StatementReply* rep) {

    auto &allNodes = programInfo->getCoverableNodes();

    int i = 1, idx = req->idx();
    for (const auto *node : allNodes) {
        if (i++ != idx)
            continue;

        const auto &srcInfo = node->getSourceInfo();
        auto sourceLine = srcInfo.toPosition().sourceLine;
        std::stringstream ss;
        ss << srcInfo.getSourceFile() << "\\" << sourceLine << ": " << *node;
        rep->set_statement(ss.str());
        break;
    }

    return Status::OK;
}

Status P4FuzzGuideImpl::GetP4Coverage(ServerContext* context,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();

    auto allNodes = programInfo->getCoverableNodes();
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
        actionBitmap = stateMgr->getActionBitmapStr();
        actionBitmapSize = stateMgr->actionBitmapSize;
    }

    newTestCase->set_stmt_cov_bitmap(stmtBitmap);
    newTestCase->set_stmt_cov_size(stmtBitmapSize);
    newTestCase->set_action_cov_bitmap(actionBitmap);
    newTestCase->set_action_cov_size(actionBitmapSize);
    newTestCase->set_table_size(tableCollector.getP4Tables().size());

    rep->set_allocated_test_case(newTestCase);

    return Status::OK;
}

Status P4FuzzGuideImpl::RecordP4Testgen(ServerContext* context,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();
    std::cout << "Record P4 Coverage of device: " << devId << std::endl;

    auto testCase = req->test_case();
    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        entity.mutable_table_entry()->set_is_valid_entry(0);
        entity.mutable_table_entry()->set_matched_idx(-1);
    }

    if (coverageMap.count(devId) == 0) {
        coverageMap.insert(std::make_pair(devId,
                    new ConcolicExecutor(*programInfo, tableCollector, top, refMap, typeMap)));
    }

    auto *stateMgr = coverageMap.at(devId);
    try {
        stateMgr->run(testCase);

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

    auto *newTestCase = new TestCase(testCase);
    newTestCase->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
    newTestCase->set_stmt_cov_size(stateMgr->statementBitmapSize);
    newTestCase->set_action_cov_bitmap(stateMgr->getActionBitmapStr());
    newTestCase->set_action_cov_size(stateMgr->actionBitmapSize);
    newTestCase->set_table_size(tableCollector.getP4Tables().size());

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

    // Get path coverage
    for (auto blockName : stateMgr->visitedPathComponents) {
        auto *pathCov = newTestCase->add_path_cov();
        pathCov->set_block_name(blockName);
        big_int totalPathNum = stateMgr->totalPaths[blockName];
        int width;
        for (width = 0; totalPathNum != 0; width++)
            totalPathNum >>= 1;

        pathCov->set_path_val(hexToByteString(
                    formatHex(stateMgr->visitedPaths[blockName], width,
                        false, true, false)));
        pathCov->set_path_size(hexToByteString(
                    formatHex(stateMgr->totalPaths[blockName], width,
                        false, true, false)));
    }

    rep->set_allocated_test_case(newTestCase);

    return Status::OK;
}

/**
 * ASYNC API
 */

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
            new GetP4StatementData(service_, cq_, programInfo_, tableCollector_);

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
            new GetP4CoverageData(service_, cq_, programInfo_, tableCollector_);

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
                actionBitmap = stateMgr->getActionBitmapStr();
                actionBitmapSize = stateMgr->actionBitmapSize;
            }

            newTestCase->set_stmt_cov_bitmap(stmtBitmap);
            newTestCase->set_stmt_cov_size(stmtBitmapSize);
            newTestCase->set_action_cov_bitmap(actionBitmap);
            newTestCase->set_action_cov_size(actionBitmapSize);
            newTestCase->set_table_size(tableCollector_.getP4Tables().size());

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
            new RecordP4TestgenData(service_, cq_, programInfo_, tableCollector_);

            devId = request_.device_id();
            status_ = CallData::REQ;
            testCase = request_.test_case();
            for (auto &entity : *testCase.mutable_entities()) {
                if (!entity.has_table_entry())
                    continue;

                entity.mutable_table_entry()->set_is_valid_entry(0);
                entity.mutable_table_entry()->set_matched_idx(-1);
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
            newTestCase->set_action_cov_bitmap(stateMgr->getActionBitmapStr());
            newTestCase->set_action_cov_size(stateMgr->actionBitmapSize);
            newTestCase->set_table_size(tableCollector_.getP4Tables().size());

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


            // Get path coverage
            for (auto blockName : stateMgr->visitedPathComponents) {
                auto *pathCov = newTestCase->add_path_cov();
                pathCov->set_block_name(blockName);
                big_int totalPathNum = stateMgr->totalPaths[blockName];
                int width;
                for (width = 0; totalPathNum != 0; width++)
                    totalPathNum >>= 1;

                pathCov->set_path_val(hexToByteString(
                            formatHex(stateMgr->visitedPaths[blockName], width,
                                false, true, false)));
                pathCov->set_path_size(hexToByteString(
                            formatHex(stateMgr->totalPaths[blockName], width,
                                false, true, false)));
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
