#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_SNAPSHOT_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_SNAPSHOT_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/visitor.h"
#include "lib/big_int.h"
#include "lib/cstring.h"

namespace p4::config::v1 {
class P4Info;
}  // namespace p4::config::v1

namespace symbex {
class CpSnapshot;
}  // namespace symbex

namespace P4::P4Tools::Symbex {

class FinalState;

/// One key field of a snapshot rule, in target-neutral form. Values are unsigned big-endian
/// integers as P4Runtime and BfRt encode them.
struct CpSnapshotKey {
    enum class Kind { Exact, Ternary, Lpm, Range, Optional };
    Kind kind = Kind::Exact;
    big_int value = 0;
    big_int mask = 0;     ///< Ternary
    int prefixLen = -1;   ///< Lpm
    big_int high = 0;     ///< Range (value is the low bound)
};

/// One table rule (or a default action, which has no keys) of a snapshot.
struct CpSnapshotRule {
    std::map<cstring, CpSnapshotKey> keys;  ///< by key field name
    cstring action;
    std::map<cstring, big_int> params;  ///< by parameter name
    /// Match priority normalized so that a LARGER value wins (P4Runtime order); BfRt's
    /// $MATCH_PRIORITY, where a smaller value wins, is negated on load.
    int64_t priority = 0;
};

struct CpSnapshotTable {
    std::vector<CpSnapshotRule> rules;
    std::optional<CpSnapshotRule> defaultAction;
};

/// Control-plane state captured from a running target (--cp-snapshot), loaded from a
/// symbex.CpSnapshot textproto. Names are kept as the snapshot spells them; lookups match an IR
/// control-plane name against them by dotted suffix, so BfRt's leading pipeline name ("pipe.",
/// "pipe1.") needs no special casing.
class CpSnapshot {
 public:
    /// Parse @p path. Raises a P4 error() and returns std::nullopt on any error.
    static std::optional<CpSnapshot> load(const std::string &path);

    [[nodiscard]] cstring target() const { return target_; }
    [[nodiscard]] cstring program() const { return program_; }

    /// The snapshot table matching control-plane name @p cpName, or nullptr. Errors when several
    /// snapshot tables match.
    [[nodiscard]] const CpSnapshotTable *table(cstring cpName) const;
    /// Names of snapshot tables that no IR table claimed (reported once the pass has run).
    [[nodiscard]] std::set<cstring> unclaimedTables() const;

    /// Register cells of register @p cpName: index -> field values (one per field, in field order;
    /// a single value for a scalar register). nullptr when the snapshot holds no cell for it.
    [[nodiscard]] const std::map<big_int, std::vector<big_int>> *registerCells(cstring cpName) const;

    /// The value the controller set for run-time parameter @p cpName (tofino RegisterParam), or
    /// nullopt when the snapshot does not set it.
    [[nodiscard]] std::optional<big_int> registerParam(cstring cpName) const;

    /// Resolve P4Runtime register ids (bmv2 RegisterEntry carries no name) through @p p4Info.
    void bindP4Info(const p4::config::v1::P4Info &p4Info);

    /// Ports of the captured topology per role; empty when the snapshot has no topology.
    [[nodiscard]] const std::set<int> &hostPorts() const { return hostPorts_; }
    [[nodiscard]] const std::set<int> &attackerPorts() const { return attackerPorts_; }
    [[nodiscard]] bool hasTopology() const { return !hostPorts_.empty() || !attackerPorts_.empty(); }

    /// Egress ports multicast group @p mgid replicates to (BfRt $pre.mgid/$pre.node or P4Runtime
    /// multicast_group_entry); nullopt when the snapshot does not define the group.
    [[nodiscard]] std::optional<std::set<int>> multicastPorts(int mgid) const;

    /// The parsed message, for emitting the snapshot block into a test file.
    [[nodiscard]] const symbex::CpSnapshot &proto() const { return *proto_; }

 private:
    cstring target_;
    cstring program_;
    std::map<cstring, CpSnapshotTable> tables_;
    mutable std::set<cstring> claimedTables_;
    std::map<cstring, std::map<big_int, std::vector<big_int>>> registers_;
    /// bmv2 register cells keyed by P4Runtime register id until bindP4Info names them.
    std::map<uint32_t, std::map<big_int, std::vector<big_int>>> registersById_;
    std::map<cstring, big_int> registerParams_;
    std::map<int, std::set<int>> multicastPorts_;
    std::set<int> hostPorts_;
    std::set<int> attackerPorts_;
    std::shared_ptr<symbex::CpSnapshot> proto_;
};

/// The snapshot named by --cp-snapshot, loaded once on first use, or nullptr when the flag is
/// absent (or the file failed to load - the error is raised at that point).
CpSnapshot *loadedCpSnapshot();

/// The loaded snapshot as `snapshot_entities { ... }` blocks of a TamperingTestCase textproto
/// (P4Runtime entities when @p bfrt is false, BfRt entities otherwise), preceded by a
/// `# cp-snapshot` comment. Empty when no snapshot is loaded.
std::string cpSnapshotTxtpbBlock(bool bfrt);

/// Closed-world materialization of a snapshot: every table without P4 const entries gets the
/// snapshot's rules as `const entries` (an empty list when the snapshot does not mention it) and
/// the snapshot's default action (else its P4 default) as `const default_action`. Both are const so
/// TableUtils::checkTableImmutability marks the table and its default immutable: symbex then
/// evaluates the entries like P4 const entries and never synthesizes an entry or a default
/// override. Run after the midend, so key names are final (tofino's RenameKeys has run).
class ApplyCpSnapshot : public Transform {
 public:
    explicit ApplyCpSnapshot(const CpSnapshot &snapshot) : snapshot(snapshot) {
        setName("ApplyCpSnapshot");
    }
    const IR::Node *preorder(IR::P4Table *table) override;
    const IR::Node *postorder(IR::P4Program *program) override;

 private:
    const CpSnapshot &snapshot;
    size_t tablesWithEntries = 0;
    size_t closedTables = 0;
    size_t entryCount = 0;
};

/// Register @p cpName's snapshot cells as (index, value) base cells. Each value is a symbolic
/// variable named "cpsnap_<register>_<index>[_<field>]", pinned to the captured value by the
/// matching `pins` constraint: the model still reads the deployed value, while the variable keeps
/// the path constraints in terms of the cell so checkRegisterKnowledge can ask what happens for
/// other values. Empty when there is nothing to seed.
struct SnapshotRegisterSeed {
    std::vector<std::pair<const IR::Expression *, const IR::Expression *>> cells;
    std::vector<const IR::Expression *> pins;
};
SnapshotRegisterSeed snapshotRegisterSeed(cstring cpName, const IR::Type *indexType,
                                          const IR::Type *type);

/// Seed @p registerValue's base cells from the snapshot and pin them in @p state.
template <class RegisterValue, class State>
void seedRegisterFromSnapshot(RegisterValue &registerValue, State &state, cstring cpName,
                              const IR::Type *indexType, const IR::Type *type) {
    auto seed = snapshotRegisterSeed(cpName, indexType, type);
    if (seed.cells.empty()) return;
    registerValue.setBaseCells(std::move(seed.cells));
    for (const auto *pin : seed.pins) state.pushPathConstraint(pin);
}

/// The prefix of the symbolic variables standing for CP-set (--cp-snapshot) values.
inline constexpr std::string_view kCpSnapshotVarPrefix = "cpsnap_";

/// The value of run-time parameter @p cpName (tofino RegisterParam) under the snapshot, as a
/// pinned symbolic variable like a register cell; nullptr when the snapshot does not set it.
const IR::Expression *snapshotRegisterParam(cstring cpName, const IR::Type *type,
                                            std::vector<const IR::Expression *> &pins);

/// Threat-model check of one tampering case under --cp-snapshot: does the attack need to know
/// CP-set values (register cells, register params) the data-plane attacker cannot read?
///
/// The CP-set values are pinned symbolic variables (see snapshotRegisterSeed). Every other variable
/// of each phase's path constraint is replaced by that phase's model value, so the two phases
/// share only the CP-set variables. The attack needs no knowledge when every deployment that
/// keeps the victim's Phase-1 path also keeps the attacker's Phase-2 path:
///   UNSAT( PC1[cp] && !PC2[cp] )  ->  "none"
///   SAT                            ->  "required: <the CP-set variables Phase 2 reads>"
/// "none" also when Phase 2 reads no CP-set value; "unknown" when the solver gives up. Empty
/// when no snapshot is loaded. Phase 3 is not covered: its registers are carried as folded
/// constants, so its dependence on CP-set values is not visible here.
cstring checkRegisterKnowledge(const FinalState &phase1, const FinalState &phase2);

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_CP_SNAPSHOT_H_ */
