#include "backends/p4tools/modules/symbex/async_server.h"

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
#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/depth_first.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/test_backend.h"

namespace P4::P4Tools::Symbex {

using namespace P4::literals;

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

// ============================================================
// SYNC API — kept as backup
// ============================================================

P4FuzzGuideImpl::P4FuzzGuideImpl(std::map<std::string, ConcolicExecutor*> &coverageMap,
        const ProgramInfo &programInfo, TableCollector &tableCollector,
        const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
        ServerState *state)
    : coverageMap(coverageMap),
      programInfo(programInfo),
      tableCollector(tableCollector),
      top(top),
      refMap(refMap),
      typeMap(typeMap),
      state(state) {}

void P4FuzzGuideImpl::requestShutdown() {
    // Lock the mutex to ensure thread-safe access
    std::lock_guard<std::mutex> lock(state->shutdown_mu);
    // Set the shutdown flag
    state->shutdown_requested = true;
}

Status P4FuzzGuideImpl::Hello(ServerContext* /* context */,
        const HealthCheckRequest* /* req */,
        HealthCheckResponse* rep) {
    rep->set_status(1);
    return Status::OK;
}

Status P4FuzzGuideImpl::GetP4Name(ServerContext* /* context */,
        const P4NameRequest *req,
        P4NameReply *rep) {

    rep->set_entity_type(req->entity_type());
    switch (req->entity_type()) {
        case 0:
            for (const auto *table : tableCollector.getP4TableSet()) {
                rep->add_name(table->controlPlaneName());
            }
            break;

        case 1:
            {
                for (const auto *table : tableCollector.getP4TableSet()) {
                    if (table->controlPlaneName() == req->target() &&
                            table->getKey() != nullptr) {
                        for (const auto *key : table->getKey()->keyElements) {
                            const IR::Expression* keyExpr = key->expression;
                            const auto* keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
                            rep->add_name(key->getAnnotation("name"_cs)->getName());
                            rep->add_type(key->matchType->toString());
                            rep->add_bit_len(keyType->width_bits());
                        }
                        break;
                    }
                }
                break;
            }

        case 2:
            {
                auto *p4TableActions = tableCollector.getActions(req->target());
                bool hasProfile = tableCollector.hasActionProfile(req->target());
                if (p4TableActions != nullptr) {
                    for (const auto *action : *p4TableActions) {
                        rep->add_name(action->checkedTo<IR::P4Action>()->controlPlaneName());
                        if (hasProfile) {
                            rep->add_bit_len(1);
                        } else {
                            rep->add_bit_len(0);
                        }
                    }
                }
                break;
            }

        case 3:
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

        default:
            break;
    }

    return Status::OK;
}

Status P4FuzzGuideImpl::GetP4Statement(ServerContext* /* context */,
        const P4StatementRequest* req,
        P4StatementReply* rep) {

    auto &allNodes = programInfo.getCoverableNodes();

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

Status P4FuzzGuideImpl::GetP4Coverage(ServerContext* /* context */,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();

    auto allNodes = programInfo.getCoverableNodes();
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

Status P4FuzzGuideImpl::GenRuleSymbex(ServerContext* /* context */,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();
    std::cout << "Record P4 Coverage of device: " << devId << std::endl;

    auto testCase = req->test_case();
    testCase.set_unsupported(0);
    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        entity.mutable_table_entry()->set_is_valid_entry(0);
        entity.mutable_table_entry()->set_matched_idx(-1);
    }

    if (coverageMap.count(devId) == 0) {
        coverageMap.insert(std::make_pair(devId,
                    new ConcolicExecutor(programInfo, tableCollector, top, refMap, typeMap)));
    }

    auto *stateMgr = coverageMap.at(devId);
    try {
        stateMgr->setGenRuleMode(true);
        stateMgr->run(testCase);

    } catch (const Util::CompilerBug &e) {
        std::cerr << "Internal compiler error: " << e.what() << std::endl;
        std::cerr << "Please submit a bug report with your code." << std::endl;
        requestShutdown();
        return Status::CANCELLED;

    } catch (const Util::CompilationError &e) {
        std::cerr << "Compilation error: " << e.what() << std::endl;
        requestShutdown();
        return Status::CANCELLED;

    } catch (SymbexUnimplemented &e) {
        std::cerr << "Unimplemented error: " << e.what() << std::endl;
        requestShutdown();
        return Status(StatusCode::UNIMPLEMENTED, "unimplemented");

    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << std::endl;
        std::cerr << "Please submit a bug report with your code." << std::endl;
        requestShutdown();
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
        if (outputPacket.getPort() != 0) {
            auto* output = newTestCase->add_expected_output_packet();
            const auto* payload = outputPacket.getEvaluatedPayload();
            const auto* payloadMask = outputPacket.getEvaluatedPayloadMask();

            output->set_port(outputPacket.getPort());
            output->set_packet(hexToByteString(formatHexExpr(payload, {false, true, false})));
            output->set_packet_mask(hexToByteString(formatHexExpr(payloadMask, {false, true, false})));
        }
    }

    newTestCase->clear_parser_states();
    for (auto stateName : stateMgr->visitedParserStates) {
        newTestCase->add_parser_states(stateName);
    }

    // Get path coverage
    newTestCase->clear_path_cov();
    std::set<cstring> visitedPath;
    for (auto blockName : stateMgr->visitedPathComponents) {
        // Skip if blockName exists
        if (visitedPath.find(blockName) != visitedPath.end())
            continue;

        // Fill path coverage in testCase
        auto *pathCov = newTestCase->add_path_cov();
        pathCov->set_block_name(blockName);
        big_int totalPathNum = stateMgr->totalPaths[blockName];
        int width;
        for (width = 0; totalPathNum != 0; width++)
            totalPathNum >>= 1;

        pathCov->set_path_val(hexToByteString(
                    formatHex(stateMgr->visitedPaths[blockName], width,
                        {false, true, false})));
        pathCov->set_path_size(hexToByteString(
                    formatHex(stateMgr->totalPaths[blockName], width,
                        {false, true, false})));

        visitedPath.insert(blockName);
    }

    rep->set_allocated_test_case(newTestCase);

    return Status::OK;
}

Status P4FuzzGuideImpl::RecordSymbex(ServerContext* /* context */,
        const P4CoverageRequest* req,
        P4CoverageReply* rep) {

    auto devId = req->device_id();
    std::cout << "Record P4 Coverage of device: " << devId << std::endl;

    auto testCase = req->test_case();
    testCase.set_unsupported(0);
    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        entity.mutable_table_entry()->set_is_valid_entry(0);
        entity.mutable_table_entry()->set_matched_idx(-1);
    }

    if (coverageMap.count(devId) == 0) {
        coverageMap.insert(std::make_pair(devId,
                    new ConcolicExecutor(programInfo, tableCollector, top, refMap, typeMap)));
    }

    auto *stateMgr = coverageMap.at(devId);
    try {
        stateMgr->setGenRuleMode(false);
        stateMgr->run(testCase);

    } catch (const Util::CompilerBug &e) {
        std::cerr << "Internal compiler error: " << e.what() << std::endl;
        std::cerr << "Please submit a bug report with your code." << std::endl;
        requestShutdown();
        return Status::CANCELLED;

    } catch (const Util::CompilationError &e) {
        std::cerr << "Compilation error: " << e.what() << std::endl;
        requestShutdown();
        return Status::CANCELLED;

    } catch (SymbexUnimplemented &e) {
        std::cerr << "Unimplemented error: " << e.what() << std::endl;
        requestShutdown();
        return Status(StatusCode::UNIMPLEMENTED, "unimplemented");

    } catch (const std::exception &e) {
        std::cerr << "Internal error: " << e.what() << std::endl;
        std::cerr << "Please submit a bug report with your code." << std::endl;
        requestShutdown();
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
        if (outputPacket.getPort() != 0) {
            auto* output = newTestCase->add_expected_output_packet();
            const auto* payload = outputPacket.getEvaluatedPayload();
            const auto* payloadMask = outputPacket.getEvaluatedPayloadMask();

            output->set_port(outputPacket.getPort());
            output->set_packet(hexToByteString(formatHexExpr(payload, {false, true, false})));
            output->set_packet_mask(hexToByteString(formatHexExpr(payloadMask, {false, true, false})));
        }
    }

    newTestCase->clear_parser_states();
    for (auto stateName : stateMgr->visitedParserStates) {
        newTestCase->add_parser_states(stateName);
    }

    // Get path coverage
    newTestCase->clear_path_cov();
    std::set<cstring> visitedPath;
    for (auto blockName : stateMgr->visitedPathComponents) {
        // Skip if blockName exists
        if (visitedPath.find(blockName) != visitedPath.end())
            continue;

        // Fill path coverage in testCase
        auto *pathCov = newTestCase->add_path_cov();
        pathCov->set_block_name(blockName);
        big_int totalPathNum = stateMgr->totalPaths[blockName];
        int width;
        for (width = 0; totalPathNum != 0; width++)
            totalPathNum >>= 1;

        pathCov->set_path_val(hexToByteString(
                    formatHex(stateMgr->visitedPaths[blockName], width,
                        {false, true, false})));
        pathCov->set_path_size(hexToByteString(
                    formatHex(stateMgr->totalPaths[blockName], width,
                        {false, true, false})));

        visitedPath.insert(blockName);
    }

    rep->set_allocated_test_case(newTestCase);

    return Status::OK;
}

// ============================================================
// ASYNC API — Proceed implementations
// ============================================================

// Helper used by RecordSymbexData and GenRuleSymbexData to fill coverage reply fields.
// Mirrors the tail section common to both sync RecordSymbex and GenRuleSymbex.
static void fillCoverageReply(TestCase *newTestCase, ConcolicExecutor *stateMgr,
                              TableCollector &tableCollector) {
    newTestCase->set_stmt_cov_bitmap(stateMgr->getStatementBitmapStr());
    newTestCase->set_stmt_cov_size(stateMgr->statementBitmapSize);
    newTestCase->set_action_cov_bitmap(stateMgr->getActionBitmapStr());
    newTestCase->set_action_cov_size(stateMgr->actionBitmapSize);
    newTestCase->set_table_size(tableCollector.getP4Tables().size());

    auto outputPacketOpt = stateMgr->getOutputPacket();
    newTestCase->clear_expected_output_packet();
    if (outputPacketOpt != boost::none) {
        auto outputPacket = outputPacketOpt.get();
        if (outputPacket.getPort() != 0) {
            auto *output = newTestCase->add_expected_output_packet();
            const auto *payload = outputPacket.getEvaluatedPayload();
            const auto *payloadMask = outputPacket.getEvaluatedPayloadMask();
            output->set_port(outputPacket.getPort());
            output->set_packet(hexToByteString(formatHexExpr(payload, {false, true, false})));
            output->set_packet_mask(hexToByteString(formatHexExpr(payloadMask, {false, true, false})));
        }
    }

    newTestCase->clear_parser_states();
    for (auto stateName : stateMgr->visitedParserStates) {
        newTestCase->add_parser_states(stateName);
    }

    newTestCase->clear_path_cov();
    std::set<cstring> visitedPath;
    for (auto blockName : stateMgr->visitedPathComponents) {
        if (visitedPath.find(blockName) != visitedPath.end())
            continue;
        auto *pathCov = newTestCase->add_path_cov();
        pathCov->set_block_name(blockName);
        big_int totalPathNum = stateMgr->totalPaths[blockName];
        int width;
        for (width = 0; totalPathNum != 0; width++)
            totalPathNum >>= 1;
        pathCov->set_path_val(hexToByteString(
                    formatHex(stateMgr->visitedPaths[blockName], width, {false, true, false})));
        pathCov->set_path_size(hexToByteString(
                    formatHex(stateMgr->totalPaths[blockName], width, {false, true, false})));
        visitedPath.insert(blockName);
    }
}

CallData::CallStatus HelloData::Proceed(
        std::map<std::string, ConcolicExecutor*> & /*coverageMap*/,
        std::string & /*devId*/, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        // ok=false here means the server is shutting down — don't re-arm.
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;
        reply_.set_status(1);
        responder_.Finish(reply_, Status::OK, this);
        return CallData::REQ;
    }
    // RET: Finish event delivered (ok=true) or client disconnected (ok=false).
    // Re-arm either way so future clients can be served.
    new HelloData(service_, cq_);
    delete this;
    return CallData::FINISH;
}

CallData::CallStatus GetP4NameData::Proceed(
        std::map<std::string, ConcolicExecutor*> & /*coverageMap*/,
        std::string & /*devId*/, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;

        reply_.set_entity_type(request_.entity_type());
        switch (request_.entity_type()) {
            case 0:
                for (const auto *table : tableCollector_.getP4TableSet()) {
                    reply_.add_name(table->controlPlaneName());
                }
                break;

            case 1:
                for (const auto *table : tableCollector_.getP4TableSet()) {
                    if (table->controlPlaneName() == request_.target() &&
                            table->getKey() != nullptr) {
                        for (const auto *key : table->getKey()->keyElements) {
                            const IR::Expression *keyExpr = key->expression;
                            const auto *keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
                            reply_.add_name(key->getAnnotation("name"_cs)->getName());
                            reply_.add_type(key->matchType->toString());
                            reply_.add_bit_len(keyType->width_bits());
                        }
                        break;
                    }
                }
                break;

            case 2:
                {
                    auto *p4TableActions = tableCollector_.getActions(request_.target());
                    bool hasProfile = tableCollector_.hasActionProfile(request_.target());
                    if (p4TableActions != nullptr) {
                        for (const auto *action : *p4TableActions) {
                            reply_.add_name(action->checkedTo<IR::P4Action>()->controlPlaneName());
                            reply_.add_bit_len(hasProfile ? 1 : 0);
                        }
                    }
                    break;
                }

            case 3:
                for (const auto *action : tableCollector_.getActionNodes()) {
                    const auto *p4Action = action->checkedTo<IR::P4Action>();
                    if (p4Action->controlPlaneName() == request_.target()) {
                        for (const auto *param : *p4Action->parameters) {
                            const auto *paramType = param->type->checkedTo<IR::Type_Bits>();
                            reply_.add_name(param->controlPlaneName());
                            reply_.add_bit_len(paramType->width_bits());
                        }
                        break;
                    }
                }
                break;

            default:
                break;
        }

        responder_.Finish(reply_, Status::OK, this);
        return CallData::REQ;
    }
    new GetP4NameData(service_, cq_, tableCollector_);
    delete this;
    return CallData::FINISH;
}

CallData::CallStatus GetP4CoverageData::Proceed(
        std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;

        devId = request_.device_id();
        auto allNodes = programInfo_.getCoverableNodes();
        std::cout << "Get P4 Coverage of device: " << devId << std::endl;

        auto *newTestCase = new TestCase(request_.test_case());
        std::string stmtBitmap, actionBitmap;
        int stmtBitmapSize, actionBitmapSize;
        if (coverageMap.count(devId) == 0) {
            stmtBitmap = "";
            stmtBitmapSize = allNodes.size();
            actionBitmap = "";
            actionBitmapSize = 0;
        } else {
            auto *stateMgr = coverageMap.at(devId);
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

        responder_.Finish(reply_, Status::OK, this);
        return CallData::REQ;
    }
    new GetP4CoverageData(service_, cq_, programInfo_, tableCollector_);
    delete this;
    return CallData::FINISH;
}

CallData::CallStatus GetP4StatementData::Proceed(
        std::map<std::string, ConcolicExecutor*> & /*coverageMap*/,
        std::string & /*devId*/, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;

        auto &allNodes = programInfo_.getCoverableNodes();
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

        responder_.Finish(reply_, Status::OK, this);
        return CallData::REQ;
    }
    new GetP4StatementData(service_, cq_, programInfo_, tableCollector_);
    delete this;
    return CallData::FINISH;
}

CallData::CallStatus RecordSymbexData::Proceed(
        std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;

        devId = request_.device_id();
        std::cout << "Record P4 Coverage of device: " << devId << std::endl;

        auto tc = request_.test_case();
        tc.set_unsupported(0);
        for (auto &entity : *tc.mutable_entities()) {
            if (!entity.has_table_entry())
                continue;
            entity.mutable_table_entry()->set_is_valid_entry(0);
            entity.mutable_table_entry()->set_matched_idx(-1);
        }

        if (coverageMap.count(devId) == 0) {
            coverageMap.insert(std::make_pair(devId,
                        new ConcolicExecutor(programInfo_, tableCollector_, top_, refMap_, typeMap_)));
        }

        auto *stateMgr = coverageMap.at(devId);
        Status grpcStatus = Status::OK;
        try {
            stateMgr->setGenRuleMode(false);
            stateMgr->run(tc);

        } catch (const Util::CompilerBug &e) {
            std::cerr << "Internal compiler error: " << e.what() << std::endl;
            std::cerr << "Please submit a bug report with your code." << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;

        } catch (const Util::CompilationError &e) {
            std::cerr << "Compilation error: " << e.what() << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;

        } catch (SymbexUnimplemented &e) {
            std::cerr << "Unimplemented error: " << e.what() << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status(StatusCode::UNIMPLEMENTED, "unimplemented");

        } catch (const std::exception &e) {
            std::cerr << "Internal error: " << e.what() << std::endl;
            std::cerr << "Please submit a bug report with your code." << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;
        }

        if (grpcStatus.ok()) {
            auto *newTestCase = new TestCase(tc);
            fillCoverageReply(newTestCase, stateMgr, tableCollector_);
            reply_.set_allocated_test_case(newTestCase);
        }

        responder_.Finish(reply_, grpcStatus, this);
        return CallData::REQ;
    }
    // RET: re-arm only if not shutting down
    if (!state_->shutdown_requested) {
        new RecordSymbexData(service_, cq_, programInfo_, tableCollector_,
                             top_, refMap_, typeMap_, state_);
    }
    delete this;
    return CallData::FINISH;
}

CallData::CallStatus GenRuleSymbexData::Proceed(
        std::map<std::string, ConcolicExecutor*> &coverageMap,
        std::string &devId, TestCase & /*testCase*/, CallStatus callStatus) {
    if (status_ == CallData::CREATE) {
        if (callStatus == CallData::ERROR) { delete this; return CallData::ERROR; }
        status_ = CallData::RET;

        devId = request_.device_id();
        std::cout << "Record P4 Coverage of device: " << devId << std::endl;

        auto tc = request_.test_case();
        tc.set_unsupported(0);
        for (auto &entity : *tc.mutable_entities()) {
            if (!entity.has_table_entry())
                continue;
            entity.mutable_table_entry()->set_is_valid_entry(0);
            entity.mutable_table_entry()->set_matched_idx(-1);
        }

        if (coverageMap.count(devId) == 0) {
            coverageMap.insert(std::make_pair(devId,
                        new ConcolicExecutor(programInfo_, tableCollector_, top_, refMap_, typeMap_)));
        }

        auto *stateMgr = coverageMap.at(devId);
        Status grpcStatus = Status::OK;
        try {
            stateMgr->setGenRuleMode(true);
            stateMgr->run(tc);

        } catch (const Util::CompilerBug &e) {
            std::cerr << "Internal compiler error: " << e.what() << std::endl;
            std::cerr << "Please submit a bug report with your code." << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;

        } catch (const Util::CompilationError &e) {
            std::cerr << "Compilation error: " << e.what() << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;

        } catch (SymbexUnimplemented &e) {
            std::cerr << "Unimplemented error: " << e.what() << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status(StatusCode::UNIMPLEMENTED, "unimplemented");

        } catch (const std::exception &e) {
            std::cerr << "Internal error: " << e.what() << std::endl;
            std::cerr << "Please submit a bug report with your code." << std::endl;
            state_->shutdown_requested = true;
            grpcStatus = Status::CANCELLED;
        }

        if (grpcStatus.ok()) {
            auto *newTestCase = new TestCase(tc);
            fillCoverageReply(newTestCase, stateMgr, tableCollector_);
            reply_.set_allocated_test_case(newTestCase);
        }

        responder_.Finish(reply_, grpcStatus, this);
        return CallData::REQ;
    }
    // RET: re-arm only if not shutting down
    if (!state_->shutdown_requested) {
        new GenRuleSymbexData(service_, cq_, programInfo_, tableCollector_,
                              top_, refMap_, typeMap_, state_);
    }
    delete this;
    return CallData::FINISH;
}

} // namespace P4::P4Tools::Symbex
