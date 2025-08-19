#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/modules/testgen/lib/test_framework.h"
#include "backends/p4tools/modules/testgen/options.h"
#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/lib/table_collector.h"
#include "backends/p4tools/modules/testgen/p4testgen.grpc.pb.h"

namespace P4Tools::P4Testgen {

using grpc::Server;
using grpc::ServerCompletionQueue;
using p4testgen::P4FuzzGuide;

/// This is main implementation of the P4Testgen tool.
class Testgen : public AbstractP4cTool<TestgenOptions> {
 protected:
    void registerTarget() override;

    int mainImpl(const CompilerResult &compilerResult) override;

    void runServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
            const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            int grpcPort);

 public:
    virtual ~Testgen() = default;

    /// Invokes P4Testgen and returns a list of abstract tests which are generated based on the
    /// input TestgenOptions. The abstract tests can be further specialized depending on the select
    /// test back end. CompilerOptions is required to invoke the correct preprocessor and P4
    /// compiler. It is assumed that `.file` in the compiler options is set.
    static std::optional<AbstractTestList> generateTests(const CompilerOptions &options,
                                                         const TestgenOptions &testgenOptions);
    /// Invokes P4Testgen and returns a list of abstract tests which are generated based on the
    /// input TestgenOptions. The abstract tests can be further specialized depending on the select
    /// test back end. CompilerOptions is required to invoke the correct P4 compiler. This function
    /// assumes that @param program is already preprocessed. P4Testgen will directly parse the input
    /// program.
    static std::optional<AbstractTestList> generateTests(std::string_view program,
                                                         const CompilerOptions &options,
                                                         const TestgenOptions &testgenOptions);

    /// Invokes P4Testgen and writes a list of abstract tests to a specified output directory which
    /// are generated based on the input TestgenOptions. The abstract tests can be further
    /// specialized depending on the select test back end. CompilerOptions is required to invoke the
    /// correct preprocessor and P4 compiler. It is assumed that `.file` in the compiler options is
    /// set.
    static int writeTests(const CompilerOptions &options, const TestgenOptions &testgenOptions);

    /// Invokes P4Testgen and writes a list of abstract tests to a specified output directory which
    /// are generated based on the input TestgenOptions. CompilerOptions is required to invoke the
    /// correct P4 compiler. This function assumes that @param program is already preprocessed.
    /// P4Testgen will directly parse the input program.
    static int writeTests(std::string_view program, const CompilerOptions &options,
                          const TestgenOptions &testgenOptions);

 private:
    std::unique_ptr<Server> server;

};

}  // namespace P4Tools::P4Testgen

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TESTGEN_H_ */
