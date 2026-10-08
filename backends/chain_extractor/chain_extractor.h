#ifndef BACKENDS_CHAIN_EXTRACTOR_CHAIN_EXTRACTOR_H_
#define BACKENDS_CHAIN_EXTRACTOR_CHAIN_EXTRACTOR_H_

#include <map>
#include <vector>

#include "backends/ifds/graphs.h"
#include "backends/chain_extractor/dependency_graph.h"
#include "backends/chain_extractor/options.h"

namespace P4::P4ChainExtractor {

using namespace ::P4::IFDS;

/// Number of chains of @graphName in the category @chains (0 when the graph has none).
size_t chain_count(const std::map<cstring, std::vector<DependencyGraphs::SOChain>> &chains,
                   cstring graphName);

}   // namespace P4::P4ChainExtractor
#endif /* BACKENDS_CHAIN_EXTRACTOR_CHAIN_EXTRACTOR_H_ */

