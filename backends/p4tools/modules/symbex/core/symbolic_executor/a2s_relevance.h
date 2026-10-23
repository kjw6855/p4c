#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_A2S_RELEVANCE_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_A2S_RELEVANCE_H_

#include <string>
#include <vector>

#include "backends/chain_extractor/dependency_graph.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_snapshot.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// One way a control-plane update reaches an A2S chain: it changes one of the chain's cp sources
/// (writeData for inconsistency, writeIdx/readIdx for residual) or a cell of its stateful object.
struct A2SRelevance {
    size_t chainId = 0;
    cstring category;  ///< chain-cache key: a2s2o, a2s2outk, a2s2outc
    cstring kind;      ///< inconsistency | residual
    cstring step;
    size_t updateIndex = 0;
    cstring table;
    cstring action;  ///< the cp source's action ("" for a register cell)
    cstring param;   ///< the cp source's param ("" for a register cell)
    std::string reason;
};

/// The updates of @p updates (one step, @p stepId) that reach @p chain. An update reaches a cp
/// source when it inserts or deletes an entry of a source table whose action is the source action,
/// or modifies one so that the action or the source param changes; a register cell of the chain's
/// stateful object reaches every kind the chain has. Same rules as tools'
/// cp_snapshot/a2s_sources.py, whose CSV has the same columns.
std::vector<A2SRelevance> a2sRelevance(
    const P4ChainExtractor::DependencyGraphs::SOChain &chain, cstring category, cstring stepId,
    const std::vector<CpStepUpdate> &updates);

/// Write @p rows as CSV (header: chain_id,category,kind,step,update_index,table,action,param,
/// reason) to @p path.
void writeA2SRelevanceCsv(const std::string &path, const std::vector<A2SRelevance> &rows);

}  // namespace P4::P4Tools::Symbex

#endif  // BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_A2S_RELEVANCE_H_
