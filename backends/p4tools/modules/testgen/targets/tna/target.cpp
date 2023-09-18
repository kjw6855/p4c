#include "backends/p4tools/modules/testgen/targets/tna/target.h"

#include <cstddef>
#include <map>
#include <vector>

#include "backends/p4tools/common/core/solver.h"
#include "ir/ir.h"
#include "lib/cstring.h"
#include "lib/exceptions.h"
#include "lib/ordered_map.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/symbolic_executor.h"
#include "backends/p4tools/modules/testgen/core/target.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/targets/tna/cmd_stepper.h"
#include "backends/p4tools/modules/testgen/targets/tna/cmd_visitor.h"
#include "backends/p4tools/modules/testgen/targets/tna/constants.h"
#include "backends/p4tools/modules/testgen/targets/tna/expr_stepper.h"
#include "backends/p4tools/modules/testgen/targets/tna/expr_visitor.h"
#include "backends/p4tools/modules/testgen/targets/tna/program_info.h"
#include "backends/p4tools/modules/testgen/targets/tna/test_backend.h"

namespace P4Tools::P4Testgen::Tna {

/* =============================================================================================
 *  TnaTestgenTarget implementation
 * ============================================================================================= */

TnaTestgenTarget::TnaTestgenTarget() : TestgenTarget("tna", "v1model") {}

void TnaTestgenTarget::make() {
    static TnaTestgenTarget *INSTANCE = nullptr;
    if (INSTANCE == nullptr) {
        INSTANCE = new TnaTestgenTarget();
    }
}

const TnaProgramInfo *TnaTestgenTarget::initProgramImpl(
    const IR::P4Program *program, const IR::Declaration_Instance *mainDecl) const {
    // The blocks in the main declaration are just the arguments in the constructor call.
    // Convert mainDecl->arguments into a vector of blocks, represented as constructor-call
    // expressions.
    std::vector<const IR::Type_Declaration *> blocks;
    argumentsToTypeDeclarations(program, mainDecl->arguments, blocks);

    // We should have six arguments.
    BUG_CHECK(blocks.size() == 6, "%1%: The TNA architecture requires 6 pipes. Received %2%.",
              mainDecl, blocks.size());

    ordered_map<cstring, const IR::Type_Declaration *> programmableBlocks;
    std::map<int, int> declIdToGress;

    // Add to parserDeclIdToGress, mauDeclIdToGress, and deparserDeclIdToGress.
    for (size_t idx = 0; idx < blocks.size(); ++idx) {
        const auto *declType = blocks.at(idx);

        auto canonicalName = ARCH_SPEC.getArchMember(idx)->blockName;
        programmableBlocks.emplace(canonicalName, declType);

        if (idx < 3) {
            declIdToGress[declType->declid] = TNA_INGRESS;
        } else {
            declIdToGress[declType->declid] = TNA_EGRESS;
        }
    }

    return new TnaProgramInfo(program, programmableBlocks, declIdToGress);
}

    TnaTestBackend *TnaTestgenTarget::getTestBackendImpl(
        const ProgramInfo &programInfo, SymbolicExecutor &symbex,
        const std::filesystem::path &testPath) const {
        return new TnaTestBackend(programInfo, symbex, testPath);
    }

    TnaCmdStepper *TnaTestgenTarget::getCmdStepperImpl(
        ExecutionState &state, AbstractSolver &solver, const ProgramInfo &programInfo) const {
        return new TnaCmdStepper(state, solver, programInfo);
    }

    TnaExprStepper *TnaTestgenTarget::getExprStepperImpl(
        ExecutionState &state, AbstractSolver &solver, const ProgramInfo &programInfo) const {
        return new TnaExprStepper(state, solver, programInfo);
    }

    TnaCmdVisitor *TnaTestgenTarget::getCmdVisitorImpl(
        ExecutionState &state, const ProgramInfo &programInfo, TestCase &testCase) const {
        return new TnaCmdVisitor(state, programInfo, testCase);
    }

    TnaExprVisitor *TnaTestgenTarget::getExprVisitorImpl(
        ExecutionState &state, const ProgramInfo &programInfo, TestCase &testCase) const {
        return new TnaExprVisitor(state, programInfo, testCase);
    }

    const ArchSpec TnaTestgenTarget::ARCH_SPEC =
        ArchSpec("Pipeline", {// parser Parser<H, M>(packet_in b,
                              //                     out H parsedHdr,
                              //                     inout M meta,
                              //                     inout standard_metadata_t standard_metadata);

                              // parser MyIngressParser(packet_in pkt,
                              //out my_ingress_headers_t  ig_hdr,
                              //out my_ingress_metadata_t ig_md,
                              //out ingress_intrinsic_metadata_t ig_intr_md)
                              {"IngressParser", {nullptr, "*hdr", "*meta", "*intr_metadata"}},
                              // control VerifyChecksum<H, M>(inout H hdr,
                              //                              inout M meta);
                              //{"VerifyChecksum", {"*hdr", "*meta"}},

                              // control Ingress<H, M>(inout H hdr,
                              //                       inout M meta,
                              //                       inout standard_metadata_t standard_metadata);

                              // control MyIngress(
                              //    inout my_ingress_headers_t  ig_hdr,
                              //    inout my_ingress_metadata_t ig_md,
                              //    in    ingress_intrinsic_metadata_t              ig_intr_md,
                              //    in    ingress_intrinsic_metadata_from_parser_t  ig_prsr_md,
                              //    inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                              //    inout ingress_intrinsic_metadata_for_tm_t       ig_tm_md)
                              {"Ingress", {"*hdr", "*meta", "*intr_metadata", "*prsr_metadata",
                                              "*dprsr_metadata", "*tm_metadata"}},

                              //control MyIngressDeparser(
                              //    packet_out pkt,
                              //    inout my_ingress_headers_t  ig_hdr,
                              //    in    my_ingress_metadata_t ig_md,
                              //    in    ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md)
                              {"IngressDeparser", {nullptr, "*hdr", "*meta", "*dprsr_metadata"}},

                              // parser MyEgressParser(
                              // packet_in pkt,
                              // out my_egress_headers_t  eg_hdr,
                              // out my_egress_metadata_t eg_md,
                              // out egress_intrinsic_metadata_t eg_intr_md)
                              {"EgressParser", {nullptr, "*hdr", "*meta", "*intr_metadata"}},
                              //control MyEgress(
                              //    inout my_egress_headers_t  eg_hdr,
                              //    inout my_egress_metadata_t eg_md,
                              //    in    egress_intrinsic_metadata_t                 eg_intr_md,
                              //    in    egress_intrinsic_metadata_from_parser_t     eg_prsr_md,
                              //    inout egress_intrinsic_metadata_for_deparser_t    eg_dprsr_md,
                              //    inout egress_intrinsic_metadata_for_output_port_t eg_oport_md)
                              {"Egress", {"*hdr", "*meta", "*intr_metadata", "*prsr_metadata",
                                              "*dprsr_metadata", "*oport_metadata"}},
                              // control ComputeChecksum<H, M>(inout H hdr,
                              //                       inout M meta);
                              //{"ComputeChecksum", {"*hdr", "*meta"}},
                              // control Deparser<H>(packet_out b, in H hdr);
                              //control MyEgressDeparser(
                              //    packet_out pkt,
                              //    inout my_egress_headers_t  eg_hdr,
                              //    in    my_egress_metadata_t eg_md,
                              //    in    egress_intrinsic_metadata_for_deparser_t eg_dprsr_md)
                              {"EgressDeparser", {nullptr, "*hdr", "*meta", "*dprsr_metadata"}}});

const ArchSpec *TnaTestgenTarget::getArchSpecImpl() const { return &ARCH_SPEC; }

}  // namespace P4Tools::P4Testgen::Tna
