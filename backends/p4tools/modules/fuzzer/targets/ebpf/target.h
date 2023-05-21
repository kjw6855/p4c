#ifndef TESTGEN_TARGETS_EBPF_TARGET_H_
#define TESTGEN_TARGETS_EBPF_TARGET_H_

#include <stdint.h>

#include <boost/filesystem.hpp>
#include <boost/optional/optional.hpp>

#include "backends/p4tools/common/core/solver.h"
#include "ir/ir.h"

#include "backends/p4tools/modules/fuzzer/core/arch_spec.h"
#include "backends/p4tools/modules/fuzzer/core/exploration_strategy/exploration_strategy.h"
#include "backends/p4tools/modules/fuzzer/core/program_info.h"
#include "backends/p4tools/modules/fuzzer/core/target.h"
#include "backends/p4tools/modules/fuzzer/lib/execution_state.h"
#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"
#include "backends/p4tools/modules/fuzzer/targets/ebpf/cmd_stepper.h"
#include "backends/p4tools/modules/fuzzer/targets/ebpf/expr_stepper.h"
#include "backends/p4tools/modules/fuzzer/targets/ebpf/program_info.h"
#include "backends/p4tools/modules/fuzzer/targets/ebpf/test_backend.h"

namespace P4Tools {

namespace P4Testgen {

namespace EBPF {

class EBPFTestgenTarget : public TestgenTarget {
 public:
    /// Registers this target.
    static void make();

 protected:
    const EBPFProgramInfo* initProgram_impl(
        const IR::P4Program* program, const IR::Declaration_Instance* mainDecl) const override;

    int getPortNumWidth_bits_impl() const override;

    EBPFTestBackend* getTestBackend_impl(const ProgramInfo& programInfo,
                                         ExplorationStrategy& symbex,
                                         const boost::filesystem::path& testPath,
                                         boost::optional<uint32_t> seed) const override;

    EBPFCmdStepper* getCmdStepper_impl(ExecutionState& state, AbstractSolver& solver,
                                       const ProgramInfo& programInfo) const override;

    EBPFExprStepper* getExprStepper_impl(ExecutionState& state, AbstractSolver& solver,
                                         const ProgramInfo& programInfo) const override;

    VisitStepper* getVisitStepper_impl(VisitState& state, const ProgramInfo& programInfo,
                                       const TestCase& testCase) const override;

    const ArchSpec* getArchSpecImpl() const override;

 private:
    EBPFTestgenTarget();

    static const ArchSpec archSpec;
};

}  // namespace EBPF

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* TESTGEN_TARGETS_EBPF_TARGET_H_ */
