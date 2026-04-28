#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_SYMBEX_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_SYMBEX_H_

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

#include "backends/p4tools/common/p4ctool.h"

#include "backends/p4tools/common/core/z3_solver.h"
#include "backends/p4tools/modules/symbex/core/target.h"
#include "backends/p4tools/modules/symbex/lib/test_framework.h"
#include "backends/p4tools/modules/symbex/options.h"
#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/lib/table_collector.h"
#include "backends/p4tools/modules/symbex/p4symbex.grpc.pb.h"

namespace P4::P4Tools::Symbex {

using grpc::Server;
using grpc::ServerCompletionQueue;
using symbex::P4FuzzGuide;

/// This is main implementation of the Symbex tool.
class Symbex : public AbstractP4cTool<SymbexOptions> {
 protected:
    void registerTarget() override;

    int mainImpl(const CompilerResult &compilerResult) override;

    void runServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
            const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            int grpcPort);

    void runAsyncServer(const ProgramInfo *programInfo, TableCollector &tableCollector,
            const IR::ToplevelBlock *top, P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
            int grpcPort);

 public:
    virtual ~Symbex() = default;

    /// Invokes Symbex and returns a list of abstract tests which are generated based on the
    /// input SymbexOptions. The abstract tests can be further specialized depending on the select
    /// test back end. CompilerOptions is required to invoke the correct preprocessor and P4
    /// compiler. It is assumed that `.file` in the compiler options is set.
    static std::optional<AbstractTestList> generateTests(const SymbexOptions &symbexOptions);
    /// Invokes Symbex and returns a list of abstract tests which are generated based on the
    /// input SymbexOptions. The abstract tests can be further specialized depending on the select
    /// test back end. CompilerOptions is required to invoke the correct P4 compiler. This function
    /// assumes that @param program is already preprocessed. Symbex will directly parse the input
    /// program.
    static std::optional<AbstractTestList> generateTests(std::string_view program,
                                                         const SymbexOptions &symbexOptions);

    /// Invokes Symbex and writes a list of abstract tests to a specified output directory which
    /// are generated based on the input SymbexOptions. The abstract tests can be further
    /// specialized depending on the select test back end. CompilerOptions is required to invoke the
    /// correct preprocessor and P4 compiler. It is assumed that `.file` in the compiler options is
    /// set.
    static int writeTests(const SymbexOptions &symbexOptions);

    /// Invokes Symbex and writes a list of abstract tests to a specified output directory which
    /// are generated based on the input SymbexOptions. This function assumes that @param program is
    /// already preprocessed. Symbex will directly parse the input program.
    static int writeTests(std::string_view program, const SymbexOptions &symbexOptions);

 private:
    std::unique_ptr<Server> server;

};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_SYMBEX_H_ */
