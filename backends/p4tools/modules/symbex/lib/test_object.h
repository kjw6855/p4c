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
    /// False when the chosen attacker value cannot actually tamper this Phase-2 packet: i.e. the
    /// register write is a program constant (not packet-controllable) AND that constant collides
    /// with a forbidden value (the Phase-1 sink-HIT key), so the sink would not flip. The
    /// tampering executor must then skip this Phase-2 packet rather than emit an un-replayable
    /// test (the emitted attacker_value must equal what the packet really writes).
    bool feasible = true;
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

    /// Returns a snapshot of this object's final state, evaluated against @param model and
    /// flattened so it can seed the *initial* state of a later symbolic phase. Used by the
    /// tampering executor to carry Phase-1 register writes into Phase-2's initial state, so
    /// Phase-2 reads see what Phase-1 actually wrote (registers are not reset between phases
    /// on hardware). Distinct from evaluate(): for stateful objects (registers) the recorded
    /// writes are folded into the object's initial value so a subsequent read returns the
    /// post-write contents without per-index Mux expressions. Default: same as evaluate().
    [[nodiscard]] virtual const TestObject *evaluateForCarry(const Model &model) const {
        return evaluate(model, /*doComplete=*/true);
    }

    /// For a stateful object already snapshotted by evaluateForCarry(): the folded scalar value, if
    /// it is a single concrete integer. Used by the multi-packet Phase-2 accumulation loop to detect
    /// a fixpoint — when replaying the attacker packet stops changing this value, no further replay
    /// will flip the downstream condition, so the loop stops. Default: nullopt (non-scalar or
    /// unsupported ⇒ the caller falls back to the --max-phase2-packets cap for termination).
    [[nodiscard]] virtual std::optional<big_int> getCarriedScalarValue() const {
        return std::nullopt;
    }

    /// Returns a copy of this carry snapshot with its folded scalar value replaced by @p value
    /// (same index, no index conditions). Used by the analytical multi-packet path to pre-set a
    /// register to init+(k-1)*delta before re-validating the write path. Default: returns this
    /// (only stateful scalar registers override it).
    [[nodiscard]] virtual const TestObject *withCarriedScalarValue(big_int value) const {
        (void)value;
        return this;
    }

    /// True when this stateful object recorded at least one write (non-empty index conditions),
    /// i.e. the packet actually wrote it. Used to detect a "priming" packet that advanced register
    /// state even though it didn't cover the full (branch-gated) write path. Default: false.
    [[nodiscard]] virtual bool wasWritten() const { return false; }

    DECLARE_TYPEINFO(TestObject);
};

/// A map of test objects.
using TestObjectMap = ordered_map<cstring, const TestObject *>;

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TEST_OBJECT_H_ */
