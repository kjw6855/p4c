#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

#include "backends/p4tools/common/p4ctool.h"
#include "ir/ir.h"

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/p4testgen.grpc.pb.h"

namespace P4Tools::P4Testgen {

using grpc::Server;
using grpc::ServerCompletionQueue;
using p4testgen::P4FuzzGuide;

/// This is main implementation of the P4Testgen tool.
class Testgen : public AbstractP4cTool<TestgenOptions> {
 protected:
    void registerTarget() override;

    int mainImpl(const IR::P4Program *program) override;

    void runServer(const ProgramInfo *programInfo, int grpcPort);

 public:
    //virtual ~Testgen() = default;
    ~Testgen();

 private:
    std::unique_ptr<ServerCompletionQueue> cq_;
    P4FuzzGuide::AsyncService service_;
    std::unique_ptr<Server> server_;
};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_ */
