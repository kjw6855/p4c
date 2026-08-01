#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// One control-plane assumption, in the style of p4v's control-plane interface (SIGCOMM'18):
/// a symbolic predicate over data-plane execution, NOT an enumeration of forwarding rules.
/// v1 understands the subset the tampering search can act on; anything else is retained
/// verbatim as `raw` with kind == Unparsed so the file stays self-documenting.
struct CpAssumeClause {
    enum class Kind {
        Unparsed,       ///< kept for the record, not enforced
        DefaultAction,  ///< default_action(t) == a
        ActionEq,       ///< action(t) == a
        ActionNeq,      ///< action(t) != a
        Hit,            ///< hit(t)
        Miss,           ///< miss(t)
    };
    Kind kind = Kind::Unparsed;
    cstring table;   ///< table the clause constrains ("" when Unparsed)
    cstring action;  ///< action name, for the *Action kinds
    cstring raw;     ///< original clause text
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

 private:
    cstring program_;
    cstring threatModel_;
    bool hasConcretePorts_ = false;
    std::map<cstring, std::set<int>> portRoles_;
    std::map<cstring, CpRegisterRule> registers_;
    std::vector<CpAssumeClause> assume_;
};

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_ANNOTATION_H_ */
