#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_OBJECT_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_OBJECT_H_
#include <map>
#include <optional>
#include <vector>

#include "backends/p4tools/common/lib/model.h"
#include "ir/ir.h"
#include "lib/big_int.h"
#include "lib/castable.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

class TestObject;  // forward-declare for AttackerControlResult

/// Result of TestObject::withAttackerValues().
/// Bundles the modified test object (register with random attacker-chosen values) together
/// with direct model overrides: a list of (SymbolicVariable, Constant) pairs.  The caller
/// applies these overrides to Phase 2's final model AFTER computeConcolicState() so that
/// the input packet produced shows the attacker-chosen value in the written field.
struct AttackerControlResult {
    /// Copy of the test object with concrete attacker-chosen values substituted.
    const TestObject *testObject;
    /// Direct model overrides: each (symVar, attackerVal) pair is applied via Model::set()
    /// after Phase 2's computeConcolicState() to inject the attacker value into the model.
    std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>> modelOverrides;
};

/* =========================================================================================
 *  Abstract Test Object Class
 * ========================================================================================= */

class TestObject : public ICastable {
 public:
    TestObject() = default;
    ~TestObject() override = default;
    TestObject(const TestObject &) = default;
    TestObject(TestObject &&) = default;
    TestObject &operator=(const TestObject &) = default;
    TestObject &operator=(TestObject &&) = default;

    /// @returns the string name of this particular test object.
    [[nodiscard]] virtual cstring getObjectName() const = 0;

    /// @returns a version of the test object where all expressions are resolved and symbolic
    /// variables are substituted according to the mapping present in the @param model.
    [[nodiscard]] virtual const TestObject *evaluate(const Model &model, bool doComplete) const = 0;

    /// Returns a copy of this test object with symbolic write values replaced by attacker-
    /// chosen constants, together with model overrides that pin those values into Phase 2's
    /// model.  Each override is a (SymbolicVariable, Constant) pair that the caller applies
    /// via Model::set() AFTER computeConcolicState() so that the input packet emitted for
    /// Phase 2 shows the attacker-chosen value in the written field.
    ///
    /// @param model            Phase 2's DFS model, used to evaluate symbolic indices and
    ///                         determine the concrete type/width of each written value.
    /// @param fixedValue       If set, use this exact value for every write instead of
    ///                         generating a random one.  Corresponds to --state-tamper-value
    ///                         on the CLI.  When the fixed value collides with one of
    ///                         @forbiddenValues, callers warn loud and continue using the
    ///                         user-supplied value verbatim (intent preservation).
    /// @param forbiddenValues  Values the attacker MUST NOT pick (because doing so would
    ///                         preserve the sink-table HIT property the user wants violated
    ///                         in Phase 3).  When non-empty, random picks loop until they
    ///                         avoid this set.
    ///
    /// Default: no-op — returns this with empty model overrides.
    [[nodiscard]] virtual AttackerControlResult withAttackerValues(
        const Model &model, std::optional<big_int> fixedValue = std::nullopt,
        const std::vector<big_int> &forbiddenValues = {}) const {
        (void)model;
        (void)fixedValue;
        (void)forbiddenValues;
        return {this, {}};
    }

    DECLARE_TYPEINFO(TestObject);
};

/// A map of test objects.
using TestObjectMap = ordered_map<cstring, const TestObject *>;

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_OBJECT_H_ */
