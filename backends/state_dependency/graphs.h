#ifndef BACKENDS_STATE_DEPENDENCY_STATE_GRAPHS_H_
#define BACKENDS_STATE_DEPENDENCY_STATE_GRAPHS_H_

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/graphviz.hpp>

#include "frontends/p4/frontend.h"
#include "frontends/p4/parserCallGraph.h"
#include "ir/ir.h"
#include "ir/visitor.h"

namespace P4 {

class ReferenceMap;
class TypeMap;

}  // namespace P4

#endif /* BACKENDS_STATE_DEPENDENCY_STATE_GRAPHS_H_ */
