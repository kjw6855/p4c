#include "backends/p4tools/modules/symbex/targets/pna/concolic.h"

#include "backends/p4tools/modules/symbex/lib/concolic.h"

namespace P4::P4Tools::Symbex::Pna {

const ConcolicMethodImpls::ImplList PnaDpdkConcolic::PNA_DPDK_CONCOLIC_METHOD_IMPLS{};

const ConcolicMethodImpls::ImplList *PnaDpdkConcolic::getPnaDpdkConcolicMethodImpls() {
    return &PNA_DPDK_CONCOLIC_METHOD_IMPLS;
}

}  // namespace P4::P4Tools::Symbex::Pna
