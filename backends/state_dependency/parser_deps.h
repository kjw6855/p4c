#ifndef BACKENDS_STATE_DEPENDENCY_PARSER_DEPS_H_
#define BACKENDS_STATE_DEPENDENCY_PARSER_DEPS_H_

#include <map>
#include <vector>

#include "backends/state_dependency/dependency_graph.h"
#include "frontends/common/resolveReferences/referenceMap.h"
#include "frontends/p4/typeMap.h"
#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4StateDependency {

/// One header field that determines a parser-derived metadata field.
struct ParserHeaderDep {
    cstring headerPath;        // field-path string (matches get_var_name), e.g. "hdr.ipv4.identification"
    const IR::Node *headerNode;  // a representative IR node for the header field (for posKey/pinning)
};

/// Result of the parser-state dependency record: for each header-derived metadata field-path, the set of
/// header fields that determine it (transitively, through the parser states), plus a representative IR node
/// for the metadata field itself. Field-path keys match the control-side variable names (get_var_name), so
/// they can seed the per-control IFDS sources directly.
struct ParserDepsRecord {
    std::map<cstring, std::vector<ParserHeaderDep>> metaToHeaders;
    std::map<cstring, const IR::Node *> metaNode;

    bool empty() const { return metaToHeaders.empty(); }
};

/// Compute the parser-state dependency record for one parser. The parser should already be unrolled
/// (acyclic); this is a flow-insensitive may-analysis (union over branches) so order does not matter.
ParserDepsRecord computeParserDeps(const IR::P4Parser *parser, P4::ReferenceMap *refMap,
                                   P4::TypeMap *typeMap);

/// Fill @chain.parserDeps from @rec: scan the chain's write/read node expressions for parser-derived
/// metadata fields (record keys) and attach a pin per determining header (writePath set from which side
/// the metadata appears on).
void attachParserDeps(DependencyGraphs::SOChain &chain, const ParserDepsRecord &rec);

}  // namespace P4::P4StateDependency

#endif /* BACKENDS_STATE_DEPENDENCY_PARSER_DEPS_H_ */
