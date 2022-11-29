#ifndef BACKENDS_P4TOOLS_COMMON_LIB_FORMULAE_H_
#define BACKENDS_P4TOOLS_COMMON_LIB_FORMULAE_H_

#include <string>

#include "gsl/gsl-lite.hpp"
#include "ir/ir.h"
#include "ir/node.h"
#include "lib/exceptions.h"
#include "lib/log.h"

namespace P4Tools {

/// Provides common functionality for implementing a thin wrapper around a 'const Node*' to enforce
/// invariants on which forms of IR nodes can inhabit implementations of this type. Implementations
/// must provide a static repOk(const Node*) function.
template <class Self, class Node = IR::Expression>
class AbstractRepCheckedNode : public ICastable {
 protected:
    gsl::not_null<const Node*> node;

    // Implicit conversions to allow implementations to be treated like a Node*.
    operator Node const*() const { return node; }
    const Node& operator*() const { return *node; }
    const Node* operator->() const { return node; }

    /// @param classDesc a user-friendly description of the class, for reporting errors to the
    ///     user.
    explicit AbstractRepCheckedNode(const Node* node, std::string classDesc) : node(node) {
        BUG_CHECK(Self::repOk(node), "%1%: Not a valid %2%.", node, classDesc);
    }
};

/// Represents a constraint that can be shipped to and asserted within a solver.
// TODO: This should implement AbstractRepCheckedNode<Constraint>.
using Constraint = IR::Expression;

/// Represents a P4 constant value.
//
// Representations of values include the following:
//   - IR::Constant for int-like values
//   - IR::BoolLiteral
// The lowest common ancestor of these two is an IR::Literal.
using Value = IR::Literal;

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_COMMON_LIB_FORMULAE_H_ */
