#include "backends/p4tools/modules/symbex/targets/ebpf/concolic.h"

#include "backends/p4tools/modules/symbex/lib/concolic.h"

namespace P4::P4Tools::Symbex::EBPF {

const ConcolicMethodImpls::ImplList EBPFConcolic::EBPF_CONCOLIC_METHOD_IMPLS{};

const ConcolicMethodImpls::ImplList *EBPFConcolic::getEBPFConcolicMethodImpls() {
    return &EBPF_CONCOLIC_METHOD_IMPLS;
}

}  // namespace P4::P4Tools::Symbex::EBPF
