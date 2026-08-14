#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lib/big_int.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// One control-plane assumption, in the style of p4v's control-plane interface (SIGCOMM'18):
/// a symbolic predicate over data-plane execution, NOT an enumeration of forwarding rules.
/// v1 understands the subset the tampering search can act on; anything else is retained
/// verbatim as `raw` with kind == Unparsed so the file stays self-documenting.
/// One term of a clause's `when` guard. A guard made only of Eq terms can be checked structurally
/// against an emitted TableConfig; anything else has to become a path constraint, because a masked
/// or ranged key cannot be compared for equality against a single concrete entry.
struct CpTerm {
    enum class Op {
        Unsupported,  ///< loaded but never enforced - an unknown op must constrain nothing
        Eq,           ///< key == value
        Neq,          ///< key != value, or action_data != rhs
        In,           ///< key in values
        Range,        ///< lo <= key <= hi
        Lpm,          ///< key & mask(prefix) == value & mask(prefix)
        Ternary,      ///< key & mask == value & mask
    };
    Op op = Op::Unsupported;
    cstring key;                 ///< key field name, when the term constrains a key
    big_int value = 0;           ///< Eq/Neq/Lpm/Ternary right-hand side
    /// True only when the file really carried a readable `value`. `value` alone cannot express
    /// "absent": it defaults to 0, and --dump-cp-stubs deliberately writes skeletons with
    /// "value": null, so an unfilled stub would otherwise read as a pin/guard against 0 - a wrong
    /// constraint dressed up as a deliberate one. Every consumer of `value` must require this.
    bool hasValue = false;
    big_int mask = 0;            ///< Ternary mask
    int prefix = -1;             ///< Lpm prefix length
    big_int lo = 0, hi = 0;      ///< Range bounds
    std::vector<big_int> values;  ///< In set
    /// Set when the term constrains action data rather than a key: action_data(<action>, <arg>).
    cstring actionDataAction;
    cstring actionDataArg;
    /// Cross-field right-hand side, e.g. {"var": "ingress_port"}. Empty when the RHS is a literal.
    cstring rhsVar;
};

struct CpAssumeClause {
    enum class Kind {
        Unparsed,       ///< kept for the record, not enforced
        DefaultAction,  ///< default_action(t) == a
        ActionEq,       ///< action(t) == a
        ActionNeq,      ///< action(t) != a
        Hit,            ///< hit(t)
        Miss,           ///< miss(t)
        WhenThen,       ///< structured when[]/then form (both tiers)
    };
    Kind kind = Kind::Unparsed;
    cstring table;   ///< table the clause constrains ("" when Unparsed)
    cstring action;  ///< action name, for the *Action kinds
    cstring raw;     ///< original clause text
    /// Structured form. `when` empty => the clause applies unconditionally to the table.
    std::vector<CpTerm> when;
    cstring thenAction;    ///< `then.action`: the action the controller pairs with this guard
    cstring thenActionNe;  ///< `then.action_ne`: an action the controller never pairs with it
    /// True when every `when` term is Eq, so the clause can be checked without the solver.
    [[nodiscard]] bool isConcrete() const {
        for (const auto &t : when) {
            if (t.op != CpTerm::Op::Eq) return false;
        }
        return true;
    }
    // Provenance - carried through so a finding can cite why an assumption was made.
    cstring source;
    cstring ref;
    cstring reason;
};

/// Ownership/authorization facts for one state object.
struct CpRegisterRule {
    /// Roles permitted to write this SO. Empty => unconstrained.
    std::vector<cstring> writableBy;
    /// Non-empty when the SO is indexed by an unspoofable principal (e.g. "ingress_port"),
    /// which makes cross-principal poisoning structurally impossible - a negative control.
    cstring partitionedBy;
    /// True when the protocol shares this SO among all participants (e.g. a Paxos instance),
    /// so a divergence caused by another participant is in-spec.
    bool shared = false;
    /// Declared or deployed value of a cell before any packet writes it. symbex otherwise seeds a
    /// first read with createTargetUninitialized (zero), which disagrees with hardware whenever the
    /// declaration says otherwise: SwitchV2P declares Register<key_pair_t,_>(SIZE, {1,0}) keys, so
    /// its check HITs under symbex and MISSes on the switch. Unset (hasInitialValue false) leaves
    /// today's behaviour untouched.
    bool hasInitialValue = false;
    big_int initialValue = 0;
};

/// Parsed `--cp-annotation` file. External to the P4 source so shared benchmark programs stay
/// pristine; see backends/state_dependency/README or the project plan for the schema.
class CpAnnotation {
 public:
    /// Parse @p path. On any error raises a P4 `error()` and returns std::nullopt - never a
    /// partially populated object.
    static std::optional<CpAnnotation> load(const std::string &path);

    [[nodiscard]] cstring program() const { return program_; }
    [[nodiscard]] cstring threatModel() const { return threatModel_; }

    /// Roles whose declared port set contains @p port. Roles declared "abstract" (no concrete
    /// ports) never match: abstract roles are for labelling/reporting, not port arithmetic.
    [[nodiscard]] std::set<cstring> rolesForPort(int port) const;

    /// True when at least one role declares concrete ports; otherwise port classification must
    /// report "unknown" rather than guessing.
    [[nodiscard]] bool hasConcretePorts() const { return hasConcretePorts_; }

    /// Rule for @p soName, or nullptr when the SO is not annotated.
    [[nodiscard]] const CpRegisterRule *registerRule(cstring soName) const;

    [[nodiscard]] const std::vector<CpAssumeClause> &assumeClauses() const { return assume_; }

    /// Clauses that constrain @p table, filtered to the enforceable kinds.
    [[nodiscard]] std::vector<const CpAssumeClause *> clausesFor(cstring table) const;

    /// The `default_action(<table>) == <action>` clause naming @p action on @p table, or nullptr.
    /// Action names are compared on the trailing dotted component as well, the same convention
    /// clausesFor uses for table names: the IR carries fully qualified action names
    /// ("SwitchV2PIngress.set_config") while annotations name actions as the control plane sees
    /// them ("set_config").
    [[nodiscard]] const CpAssumeClause *defaultActionClause(cstring table, cstring action) const;

 private:
    cstring program_;
    cstring threatModel_;
    bool hasConcretePorts_ = false;
    std::map<cstring, std::set<int>> portRoles_;
    std::map<cstring, CpRegisterRule> registers_;
    std::vector<CpAssumeClause> assume_;
};

/// The annotation named by --cp-annotation, loaded once on first use, or nullptr when the flag is
/// absent (or the file failed to load - the error is raised at that point). Shared so the tampering
/// tracker and the table steppers see the same object.
const CpAnnotation *loadedCpAnnotation();

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_ */
