#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_ASYNC_SERVER_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_ASYNC_SERVER_H_

//#include <grpc/grpc.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpc/support/log.h>

#include "backends/p4tools/modules/testgen/core/concolic_executor/concolic_executor.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/p4testgen.grpc.pb.h"

namespace P4Tools::P4Testgen {

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using p4testgen::P4FuzzGuide;
using p4testgen::HealthCheckRequest;
using p4testgen::HealthCheckResponse;
using p4testgen::P4NameRequest;
using p4testgen::P4NameReply;
using p4testgen::P4CoverageRequest;
using p4testgen::P4CoverageReply;
using p4testgen::P4StatementRequest;
using p4testgen::P4StatementReply;
using p4testgen::TestCase;

class P4FuzzGuideImpl final : public P4FuzzGuide::Service {
 public:
    P4FuzzGuideImpl(std::map<std::string, ConcolicExecutor*> &coverageMap,
            const ProgramInfo *programInfo, TableCollector &tableCollector,
            const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap);

    Status Hello(ServerContext* context,
            const HealthCheckRequest* req,
            HealthCheckResponse* rep) override;

    Status GetP4Name(ServerContext *context,
            const P4NameRequest *req,
            P4NameReply *rep) override;

    Status GetP4Statement(ServerContext* context,
            const P4StatementRequest* req,
            P4StatementReply* rep) override;

    Status GetP4Coverage(ServerContext* context,
            const P4CoverageRequest* req,
            P4CoverageReply* rep) override;

    Status RecordP4Testgen(ServerContext* context,
            const P4CoverageRequest* req,
            P4CoverageReply* rep) override;

 private:
    std::map<std::string, ConcolicExecutor*> &coverageMap;
    const ProgramInfo *programInfo;
    TableCollector &tableCollector;
    const IR::ToplevelBlock *top;
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;

    //std::string hexToByteString(const std::string &hex);
};

class CallData {
 public:
    enum CallStatus { CREATE, REQ, RET, ERROR, FINISH };

    virtual CallStatus Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) = 0;
};

class GetP4StatementData : public CallData {
 public:
    explicit GetP4StatementData(P4FuzzGuide::AsyncService *service,
            ServerCompletionQueue *cq, const ProgramInfo *programInfo,
            TableCollector &tableCollector)
    : service_(service), cq_(cq), responder_(&ctx_), status_(CallData::CREATE), programInfo_(programInfo), tableCollector_(tableCollector) {
        service_->RequestGetP4Statement(&ctx_, &request_, &responder_,
                cq_, cq_, this);
    }

    CallStatus Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) override;

 private:
    P4FuzzGuide::AsyncService *service_;
    ServerCompletionQueue *cq_;
    ServerAsyncResponseWriter<P4StatementReply> responder_;
    CallStatus status_;  // The current serving state.
    const ProgramInfo* programInfo_;
    TableCollector &tableCollector_;
    ServerContext ctx_;
    P4StatementRequest request_;
    P4StatementReply reply_;
};

class GetP4CoverageData : public CallData {
 public:
    explicit GetP4CoverageData(P4FuzzGuide::AsyncService *service,
            ServerCompletionQueue *cq, const ProgramInfo *programInfo,
            TableCollector &tableCollector)
    : service_(service), cq_(cq), responder_(&ctx_), status_(CallData::CREATE), programInfo_(programInfo), tableCollector_(tableCollector) {
        service_->RequestGetP4Coverage(&ctx_, &request_, &responder_,
                cq_, cq_, this);
    }

    CallStatus Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) override;

 private:
    P4FuzzGuide::AsyncService *service_;
    ServerCompletionQueue *cq_;
    ServerAsyncResponseWriter<P4CoverageReply> responder_;
    CallStatus status_;  // The current serving state.
    const ProgramInfo* programInfo_;
    TableCollector &tableCollector_;
    ServerContext ctx_;
    P4CoverageRequest request_;
    P4CoverageReply reply_;
};

class RecordP4TestgenData : public CallData {
 public:
    explicit RecordP4TestgenData(P4FuzzGuide::AsyncService *service,
            ServerCompletionQueue *cq, const ProgramInfo *programInfo,
            TableCollector &tableCollector)
    : service_(service), cq_(cq), responder_(&ctx_), status_(CallData::CREATE), programInfo_(programInfo), tableCollector_(tableCollector) {
        service_->RequestRecordP4Testgen(&ctx_, &request_, &responder_,
                cq_, cq_, this);
    }

    CallStatus Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) override;

 private:
    P4FuzzGuide::AsyncService *service_;
    ServerCompletionQueue *cq_;
    ServerAsyncResponseWriter<P4CoverageReply> responder_;
    CallStatus status_;  // The current serving state.
    const ProgramInfo* programInfo_;
    TableCollector &tableCollector_;
    ServerContext ctx_;
    P4CoverageRequest request_;
    P4CoverageReply reply_;
};

class HelloData : public CallData {
 public:
    explicit HelloData(P4FuzzGuide::AsyncService *service,
            ServerCompletionQueue *cq)
    : service_(service), cq_(cq), responder_(&ctx_), status_(CallData::CREATE) {
        service_->RequestHello(&ctx_, &request_, &responder_,
                cq_, cq_, this);
    }

    CallStatus Proceed(std::map<std::string, ConcolicExecutor*> &coverageMap,
            std::string &devId, TestCase &testCase, CallStatus callStatus) override;


 private:
    P4FuzzGuide::AsyncService *service_;
    ServerCompletionQueue *cq_;
    ServerAsyncResponseWriter<HealthCheckResponse> responder_;
    CallStatus status_;  // The current serving state.
    ServerContext ctx_;
    HealthCheckRequest request_;
    HealthCheckResponse reply_;
};

} // namespace P4Tools::P4Testgen

#endif /*BACKENDS_P4TOOLS_MODULES_TESTGEN_ASYNC_SERVER_H_ */

