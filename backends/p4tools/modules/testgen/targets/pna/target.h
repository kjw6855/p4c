#ifndef BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_PNA_TARGET_H_
#define BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_PNA_TARGET_H_

#include <cstdint>
#include <filesystem>
#include <optional>

#include "backends/p4tools/common/core/solver.h"
#include "backends/p4tools/common/lib/arch_spec.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/targets/pna/dpdk/cmd_stepper.h"
#include "backends/p4tools/modules/testgen/targets/pna/dpdk/expr_stepper.h"
#include "backends/p4tools/modules/testgen/targets/pna/dpdk/program_info.h"
#include "backends/p4tools/modules/testgen/targets/pna/test_backend.h"

namespace P4Tools::P4Testgen::Pna {

class PnaDpdkTestgenTarget : public TestgenTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const PnaDpdkProgramInfo *initProgramImpl(
        const IR::P4Program *program, const IR::Declaration_Instance *mainDecl) const override;

    [[nodiscard]] int getPortNumWidthBitsImpl() const override;

    PnaTestBackend *getTestBackendImpl(const ProgramInfo &programInfo, SymbolicExecutor &symbex,
                                       const std::filesystem::path &testPath,
                                       std::optional<uint32_t> seed) const override;

    PnaDpdkCmdStepper *getCmdStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                         const ProgramInfo &programInfo) const override;

    PnaDpdkExprStepper *getExprStepperImpl(ExecutionState &state, AbstractSolver &solver,
                                           const ProgramInfo &programInfo) const override;

    CmdVisitor *getCmdVisitorImpl(ExecutionState &state, AbstractSolver &solver,
                                         const ProgramInfo &programInfo, const TestCase &testCase) const override;

    ExprVisitor *getExprVisitorImpl(ExecutionState &state, AbstractSolver &solver,
                                           const ProgramInfo &programInfo, const TestCase &testCase) const override;

    [[nodiscard]] const ArchSpec *getArchSpecImpl() const override;

 private:
    PnaDpdkTestgenTarget();

    static const ArchSpec ARCH_SPEC;
};

}  // namespace P4Tools::P4Testgen::Pna

#endif /* BACKENDS_P4TOOLS_MODULES_TESTGEN_TARGETS_PNA_TARGET_H_ */
