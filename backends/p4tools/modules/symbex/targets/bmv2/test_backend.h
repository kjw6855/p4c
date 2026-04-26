#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_BACKEND_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_BACKEND_H_

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "backends/p4tools/common/lib/trace_event.h"
#include "ir/ir.h"
#include "lib/big_int_util.h"

#include "backends/p4tools/modules/symbex/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/test_backend.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/targets/bmv2/program_info.h"

namespace P4::P4Tools::Symbex::Bmv2 {

class Bmv2TestBackend : public TestBackEnd {
 private:
    /// List of the supported back ends.
    static const std::set<std::string> SUPPORTED_BACKENDS;

 public:
    explicit Bmv2TestBackend(const Bmv2V1ModelProgramInfo &programInfo,
                             const TestBackendConfiguration &testBackendConfiguration,
                             SymbolicExecutor &symbex);

    TestBackEnd::TestInfo produceTestInfo(
        const ExecutionState *executionState, const Model *finalModel,
        const IR::Expression *outputPacketExpr, const IR::Expression *outputPortExpr,
        const std::vector<std::reference_wrapper<const TraceEvent>> *programTraces) override;

    const TestSpec *createTestSpec(const ExecutionState *executionState, const Model *finalModel,
                                   const TestInfo &testInfo) override;
};

}  // namespace P4::P4Tools::Symbex::Bmv2

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_TARGETS_BMV2_TEST_BACKEND_H_ */
