#ifndef BACKENDS_STATE_DEPENDENCY_CHAIN_CACHE_H_
#define BACKENDS_STATE_DEPENDENCY_CHAIN_CACHE_H_

#include <string>

#include "backends/state_dependency/analysis.h"
#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4StateDependency {

/// Serialize/cache the state-dependency SOChains so the expensive IFDS analysis runs once (in the
/// p4c_state_dependency binary) and p4symbex can reuse the result.
///
/// A chain is fully described, for p4symbex's purposes, by source positions + a few cstrings: symbex
/// matches chain nodes to the IR it executes by source position only (CoverageSet/SourceIdCmp compare
/// srcInfo; getConditionVar keys on the source-position string), never by pointer identity. So the
/// cache stores, per chain, the (file,line,column) of each write/read node and of the condition node,
/// plus soName/sinkTable/sinkKey/isUpdate/id. On load the positions are re-resolved to real IR::Node*
/// in the fresh program IR.

/// Deterministic content hash of the .p4 source file + arch + langVersion. Embedded in the cache
/// header by the writer and re-checked by the reader (hard error on mismatch). Both the binary and
/// p4symbex must compute it identically for the same program.
cstring computeSourceHash(const std::string &p4File, cstring arch, cstring langVersion);

/// Write the Key (h2s2k) + Cond (h2s2c) chains of @p result to @p path as JSON.
void serializeChainCache(const StateDependencyResult &result, const std::string &path,
                         cstring srcHash, cstring arch);

/// Load chains from @p path and re-resolve their source positions against @p program's IR. Only the
/// fields p4symbex consumes are populated (dataWriteKeyChains, dataWriteCondChains). Raises a P4
/// error (and returns an empty result) when the file is missing/unparseable or its header srcHash /
/// arch does not match @p srcHash / @p arch.
StateDependencyResult loadChainCache(const std::string &path, const IR::P4Program *program,
                                     cstring srcHash, cstring arch);

}  // namespace P4::P4StateDependency

#endif  // BACKENDS_STATE_DEPENDENCY_CHAIN_CACHE_H_
