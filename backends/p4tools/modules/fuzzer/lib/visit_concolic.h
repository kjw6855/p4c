#ifndef BACKENDS_P4TOOLS_MODULES_FUZZER_LIB_VISIT_CONCOLIC_H_
#define BACKENDS_P4TOOLS_MODULES_FUZZER_LIB_VISIT_CONCOLIC_H_

#include <sys/types.h>

#include <functional>
#include <list>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/variant/variant.hpp>

#include "backends/p4tools/common/lib/formulae.h"
#include "backends/p4tools/common/lib/model.h"
#include "gsl/gsl-lite.hpp"
#include "ir/ir.h"
#include "ir/vector.h"
#include "ir/visitor.h"
#include "lib/cstring.h"

#include "backends/p4tools/modules/fuzzer/lib/visit_state.h"

namespace P4Tools {

namespace P4Testgen {

/// TODO: This is a very ugly data structure. Essentially, you can store both state variables and
/// entire expression as keys. State variables can actually compared, expressions are always unique
/// keys. Using this map, you can look up particular state variables and check whether they actually
/// are present, but not expressions. The reason expressions need to be keys is that some times
/// entire expressions are mapped to a particular constant.
using VisitConcolicVariableMap =
    std::map<boost::variant<const StateVariable, const IR::Expression*>, const IR::Expression*>;

/// Encapsulates a set of concolic method implementations.
class VisitConcolicMethodImpls {
 private:
    using MethodImpl = std::function<void(
        cstring concolicMethodName, const IR::ConcolicVariable* var, const VisitState& state,
        const Model* completedModel, VisitConcolicVariableMap* resolvedVisitConcolicVariables)>;

    std::map<cstring, std::map<uint, std::list<std::pair<std::vector<cstring>, MethodImpl>>>> impls;

    static bool matches(const std::vector<cstring>& paramNames,
                        const IR::Vector<IR::Argument>* args);

 public:
    using ImplList = std::list<std::tuple<cstring, std::vector<cstring>, MethodImpl>>;

    explicit VisitConcolicMethodImpls(const ImplList& implList);

    bool exec(cstring concolicMethodName, const IR::ConcolicVariable* var,
              const VisitState& state, const Model* completedModel,
              VisitConcolicVariableMap* resolvedVisitConcolicVariables) const;

    void add(const ImplList& implList);
};

class VisitConcolicResolver : public Inspector {
 public:
    explicit VisitConcolicResolver(const Model* completedModel, const VisitState& state,
                              const VisitConcolicMethodImpls* concolicMethodImpls);

    VisitConcolicResolver(const Model* completedModel, const VisitState& state,
                     const VisitConcolicMethodImpls* concolicMethodImpls,
                     VisitConcolicVariableMap resolvedVisitConcolicVariables);

    const VisitConcolicVariableMap* getResolvedVisitConcolicVariables() const;

 private:
    bool preorder(const IR::ConcolicVariable* var) override;

    /// Execution state may be used by concolic implementations to access specific state.
    const VisitState& state;

    /// The completed model is queried to produce a (random) assignment for concolic inputs.
    gsl::not_null<const Model*> completedModel;

    /// A map of the concolic variables and the assertion associated with the variable. These
    /// assertions are used to add constraints to the solver.
    VisitConcolicVariableMap resolvedVisitConcolicVariables;

    /// A pointer to the list of implemented concolic methods. This is assembled by the testgen
    /// targets.
    gsl::not_null<const VisitConcolicMethodImpls*> concolicMethodImpls;
};

class VisitConcolic {
 public:
    static const VisitConcolicMethodImpls::ImplList* getCoreVisitConcolicMethodImpls();
};

}  // namespace P4Testgen

}  // namespace P4Tools

#endif /* BACKENDS_P4TOOLS_MODULES_FUZZER_LIB_VISIT_CONCOLIC_H_ */
