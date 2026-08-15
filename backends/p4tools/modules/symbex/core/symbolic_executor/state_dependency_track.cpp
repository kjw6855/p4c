#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/solver.h"
#include <fstream>
#include <sstream>

#include "backends/p4tools/common/lib/constants.h"
#include "lib/error.h"
#include "lib/timer.h"

#include "backends/p4tools/common/control_plane/symbolic_variables.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/variables.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_annotation.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/sink_divergence.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/core/small_step/table_stepper.h"
#include "backends/p4tools/modules/symbex/lib/concolic.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

/// Trailing-component control-plane name comparison. Defined further down (the annotation helpers
/// own it); forward-declared because the cross-phase default-action helpers below need it first.
static bool cpNameMatches(cstring full, cstring want);

/// One phase's default-action override for one table.
struct DefaultActionRecord {
    /// The evaluated override: which action this phase installed and the action data its model
    /// chose for it.
    const ActionCall *call = nullptr;
    /// Control-plane names of the arguments whose symbol appears in THIS phase's path constraint,
    /// i.e. the ones the path actually branched on. An argument outside this set was never read:
    /// its value in the model is Z3's arbitrary completion of a free symbol, and the phase stays
    /// satisfiable under any other value. A cross-phase difference there is vacuous rather than
    /// contradictory, which is why the consistency check below ignores it.
    std::set<cstring> constrainedArgs;
};

/// One KEYED table entry a phase installed: the evaluated match values that select it, plus the
/// action and action data installed behind them. tableKeyMap below keeps only the match values,
/// which is all the NEQ/EQ steering needs but leaves the entry's *contents* uncompared — and an
/// entry is what the harness installs, key and action data together.
struct TableEntryRecord {
    /// keyName → concrete evaluated match (all match kinds).
    TableMatchMap matches;
    int priority = 0;
    const ActionCall *call = nullptr;
    /// Same provenance as DefaultActionRecord::constrainedArgs, and read for the same reason.
    std::set<cstring> constrainedArgs;
};

/// Two evaluated ActionCalls install the same thing: same action, same data for every argument.
/// Positional comparison, because both calls were built from the same IR::P4Action's parameter
/// list and therefore carry their arguments in that order.
static bool sameEvaluatedActionCall(const ActionCall *a, const ActionCall *b) {
    if (a == nullptr || b == nullptr) return a == b;
    if (a->getActionName() != b->getActionName()) return false;
    const auto *args = a->getArgs();
    const auto *otherArgs = b->getArgs();
    if ((args == nullptr) != (otherArgs == nullptr)) return false;
    if (args == nullptr) return true;
    if (args->size() != otherArgs->size()) return false;
    for (size_t i = 0; i < args->size(); ++i) {
        if ((*args)[i].getActionParamName() != (*otherArgs)[i].getActionParamName()) return false;
        if ((*args)[i].getEvaluatedValue()->value != (*otherArgs)[i].getEvaluatedValue()->value) {
            return false;
        }
    }
    return true;
}

/// True when two records describe the SAME installed entry — identical match values on every key at
/// the same priority — so one device cannot hold both unless their contents agree.
///
/// Different KEYS are deliberately NOT a conflict. The tampering generator drives Phase 2's key
/// away from Phase 1's on purpose (buildTableKeyNeqConstraint), and two distinct entries of one
/// table may legitimately carry different actions and different action data; only entries the
/// control plane cannot tell apart have to agree.
static bool sameInstalledEntry(const TableEntryRecord &a, const TableEntryRecord &b) {
    if (a.priority != b.priority) return false;
    if (a.matches.size() != b.matches.size()) return false;
    for (const auto &[keyName, match] : a.matches) {
        auto it = b.matches.find(keyName);
        if (it == b.matches.end()) return false;
        // isEqualTo compares the whole match: value for exact, value+mask for ternary,
        // value+prefix for LPM, and a mismatched match kind compares unequal.
        if (!match->isEqualTo(it->second)) return false;
    }
    return true;
}

struct PhaseConditions {
    int inputPort = -1;
    int outputPort = -1;
    // tableName → keyName → concrete evaluated match (all match kinds)
    std::map<cstring, std::map<cstring, const TableMatch *>> tableKeyMap;
    // tableName → the evaluated default-action override this phase installed. A table resolved via
    // setTableDefaultEntries (every keyless table) yields a TableConfig with ZERO rules and this
    // property instead, so the tableKeyMap loop above can never see it — which is exactly how
    // keyless tables escaped every cross-phase pin.
    std::map<cstring, DefaultActionRecord> tableDefaultActionMap;
    // tableName → the KEYED entries this phase installed, whole. tableKeyMap flattens the rules of
    // a table into one keyName → match map, which loses both the entry boundaries and the action
    // data; this keeps them so a later phase's entry can be matched against the Phase-1 entry that
    // carries the same key.
    std::map<cstring, std::vector<TableEntryRecord>> tableEntryMap;

    bool operator==(const PhaseConditions &other) const {
        if (inputPort != other.inputPort || outputPort != other.outputPort) return false;
        if (tableKeyMap.size() != other.tableKeyMap.size()) return false;
        for (const auto &[tblName, keyMap] : tableKeyMap) {
            auto it = other.tableKeyMap.find(tblName);
            if (it == other.tableKeyMap.end()) return false;
            if (keyMap.size() != it->second.size()) return false;
            for (const auto &[keyName, match] : keyMap) {
                auto kit = it->second.find(keyName);
                if (kit == it->second.end()) return false;
                if (!match->isEqualTo(kit->second)) return false;
            }
        }
        // Two Phase-1 states that installed DIFFERENT default action data are different device
        // configurations, so they must bucket separately: the bucket's representative is what the
        // cross-phase consistency check compares later phases against, and lumping states with
        // different defaults together would check Phase 2 against a default its own Phase 1 never
        // installed.
        if (tableDefaultActionMap.size() != other.tableDefaultActionMap.size()) return false;
        for (const auto &[tblName, rec] : tableDefaultActionMap) {
            auto it = other.tableDefaultActionMap.find(tblName);
            if (it == other.tableDefaultActionMap.end()) return false;
            if (!sameEvaluatedActionCall(rec.call, it->second.call)) return false;
        }
        // Same argument one level down, for KEYED entries: two Phase-1 states whose entries carry
        // the same keys but different action data are different device configurations. Without
        // this they share a bucket, and the bucket's representative — the state every later phase
        // is checked against — would speak for an entry its bucket mates never installed.
        if (tableEntryMap.size() != other.tableEntryMap.size()) return false;
        for (const auto &[tblName, entries] : tableEntryMap) {
            auto it = other.tableEntryMap.find(tblName);
            if (it == other.tableEntryMap.end()) return false;
            if (entries.size() != it->second.size()) return false;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (!sameInstalledEntry(entries[i], it->second[i])) return false;
                if (!sameEvaluatedActionCall(entries[i].call, it->second[i].call)) return false;
            }
        }
        return true;
    }
};

/// True when a --cp-annotation `action_data` clause fixed (@p tableName, @p actionName, @p arg) to a
/// literal value. This is the ONLY provenance that licenses carrying a control-plane action-data
/// value from one phase into another: the annotation states what the controller installs, so every
/// phase reaches that same value on its own (TableStepper::cpActionArgPin re-applies the clause in
/// each phase's query) and nothing is invented. Matching mirrors cpActionArgPin exactly, including
/// the hasValue requirement — a `"value": null` stub pins nothing.
static bool argIsAnnotationBacked(cstring tableName, cstring actionName, const ActionArg &arg) {
    const auto *ann = loadedCpAnnotation();
    if (ann == nullptr) return false;
    const auto *param = arg.getActionParam();
    if (param == nullptr) return false;
    const cstring cpName = arg.getActionParamName();
    for (const auto *c : ann->clausesFor(tableName)) {
        for (const auto &t : c->when) {
            if (t.op != CpTerm::Op::Eq || t.actionDataArg.isNullOrEmpty() || !t.hasValue) continue;
            if (!t.actionDataAction.isNullOrEmpty() &&
                !cpNameMatches(actionName, t.actionDataAction)) {
                continue;
            }
            if (t.actionDataArg != cpName && t.actionDataArg != param->name.name) continue;
            return true;
        }
    }
    return false;
}

/// The annotation-backed subset of @p call: only the arguments a --cp-annotation `action_data`
/// clause fixed. nullptr when the call carries no such argument, in which case NOTHING from it may
/// be propagated to another phase — the remaining values are free symbols whose model assignment is
/// an arbitrary solver pick, not a control-plane fact.
static const ActionCall *annotationBackedSubset(cstring tableName, const ActionCall *call) {
    if (call == nullptr) return nullptr;
    const auto *args = call->getArgs();
    if (args == nullptr) return nullptr;
    std::vector<ActionArg> pinnable;
    for (const auto &arg : *args) {
        if (argIsAnnotationBacked(tableName, call->getActionName(), arg)) pinnable.push_back(arg);
    }
    if (pinnable.empty()) return nullptr;
    return new ActionCall(call->getActionName(), call->getAction(), pinnable);
}

/// The control-plane configuration one phase's terminal describes: the default-action override it
/// installed per table, and the keyed entries it installed per table.
struct PhaseCpState {
    std::map<cstring, DefaultActionRecord> defaults;
    std::map<cstring, std::vector<TableEntryRecord>> entries;
};

/// Everything a phase installed on the control plane, with the provenance needed to tell a real
/// cross-phase contradiction from an irrelevant difference between two arbitrary model completions.
///
/// Both halves are read from the same evaluated TableConfig: a default-action override lives in the
/// "overriden_default_action" property and carries no rules, while a keyed entry IS a rule. One
/// table can only ever be one of the two on a given path, but the loop does not need to care.
static PhaseCpState collectCpState(const FinalState &fs) {
    PhaseCpState out;
    const auto &model = fs.getFinalModel();
    const auto *es = fs.getExecutionState();
    // Symbols this phase's path actually branched on. Collected once for the whole path constraint
    // rather than per table, which would rescan it for every installed action.
    std::set<cstring> constrained;
    for (const auto *pc : es->getPathConstraint()) {
        if (pc == nullptr) continue;
        forAllMatching<IR::SymbolicVariable>(
            pc, [&](const IR::SymbolicVariable *var) { constrained.insert(var->label); });
    }
    // Which of @p call's arguments this path branched on. Both the keyed and the keyless producer
    // mint the symbol through ControlPlaneState::getTableActionArgument(table, action,
    // parameter->name, type), which labels it "<table>_<action>_arg_<param>", so one reconstruction
    // serves both.
    auto collectConstrainedArgs = [&constrained](cstring tblName, const ActionCall *call) {
        std::set<cstring> out;
        const auto *args = call->getArgs();
        if (args == nullptr) return out;
        for (const auto &arg : *args) {
            const auto *param = arg.getActionParam();
            if (param == nullptr) continue;
            const cstring label =
                tblName + "_" + call->getActionName() + "_arg_" + param->name.name;
            if (constrained.count(label) != 0) out.insert(arg.getActionParamName());
        }
        return out;
    };
    for (const auto &[tblName, tblObj] : es->getTestObjectCategory("tableconfigs"_cs)) {
        const auto *evaluated = tblObj->evaluate(model, /*doComplete=*/true);
        const auto *cfg = evaluated->to<TableConfig>();
        if (cfg == nullptr) continue;
        for (const auto &rule : *cfg->getRules()) {
            const auto *call = rule.getActionCall();
            if (call == nullptr) continue;
            TableEntryRecord rec;
            rec.matches = *rule.getMatches();
            rec.priority = rule.getPriority();
            rec.call = call;
            rec.constrainedArgs = collectConstrainedArgs(tblName, call);
            out.entries[tblName].push_back(rec);
        }
        const auto *defProperty = cfg->getProperty("overriden_default_action"_cs, /*checked=*/false);
        if (defProperty == nullptr) continue;
        const auto *call = defProperty->to<ActionCall>();
        if (call == nullptr) continue;
        DefaultActionRecord rec;
        rec.call = call;
        rec.constrainedArgs = collectConstrainedArgs(tblName, call);
        out.defaults[tblName] = rec;
    }
    return out;
}

// Build a PhaseConditions from a terminal FinalState, extracting concrete port values and
// all table key concrete matches (any match kind) from the evaluated tableconfigs test objects.
static PhaseConditions buildPhaseCondition(const FinalState &fs, const ProgramInfo &programInfo) {
    PhaseConditions cond;
    const auto &model = fs.getFinalModel();
    const auto *es = fs.getExecutionState();
    cond.inputPort  = IR::getIntFromLiteral(
        model.evaluate(es->get(programInfo.getTargetInputPortVar()),  true));
    cond.outputPort = IR::getIntFromLiteral(
        model.evaluate(es->get(programInfo.getTargetOutputPortVar()), true));
    // One pass over the evaluated tableconfigs yields both halves: the keyed entries (rules) and
    // the default-action overrides, which carry no rules at all because their action and data live
    // in a table property instead.
    auto cpState = collectCpState(fs);
    cond.tableDefaultActionMap = std::move(cpState.defaults);
    cond.tableEntryMap = std::move(cpState.entries);
    // The flattened key view the NEQ/EQ steering and the size-1 detection read. Entries are visited
    // in rule order, so a table with several entries keeps the last one's match per key — exactly
    // what this loop produced before the entry records existed.
    for (const auto &[tblName, entries] : cond.tableEntryMap) {
        for (const auto &entry : entries) {
            for (const auto &[keyName, match] : entry.matches) {
                cond.tableKeyMap[tblName][keyName] = match;
            }
        }
    }
    return cond;
}

/// Carry a table's default-action override into a later phase's initial state — but ONLY the part
/// of it that a --cp-annotation clause fixed. The device installs ONE default action per table for
/// all three phases, and the action-data symbols (`<table>_<action>_arg_<param>`) are not
/// phase-scoped while every phase is a SEPARATE solver query, so the phases can disagree on the very
/// action data the attack depends on. The answer is NOT to propagate whichever value Z3 happened to
/// pick for a free symbol: that manufactures a control-plane state nobody asked for and may not even
/// be deployable (SwitchV2P's switch_type is `enum bit<3>` with five declared roles, so a propagated
/// 6 is merely representable, not installable). Only an annotation knows what the controller
/// installs, so only an annotation-backed value is carried; everything else is left free and checked
/// afterwards by crossPhaseCpAgrees, which drops the candidate when the phases contradict.
/// Consumed by TableStepper::setTableDefaultEntries, the single producer of those symbols on both
/// targets. Kept out of "preexisting_tableconfigs": that category is read only on the keyed path,
/// and a keyless table — the whole point here — never gets there.
static void injectPinnedDefaultAction(ExecutionState &init, cstring tableName,
                                      const TableConfig *cfg) {
    if (!SymbexOptions::get().crossPhaseDefaultActionPin || cfg == nullptr) return;
    const auto *defProperty = cfg->getProperty("overriden_default_action"_cs, /*checked=*/false);
    if (defProperty == nullptr) return;
    const auto *pinnable = annotationBackedSubset(tableName, defProperty->to<ActionCall>());
    if (pinnable != nullptr) {
        init.addTestObject("pinned_default_actions"_cs, tableName, pinnable);
    }
}

/// Same pin, driven from an already-built PhaseConditions: for EVERY table that carries a default
/// override, not just the size-1 tables — a keyless table has no key and so never appears there.
static void injectPinnedDefaultActions(ExecutionState &init, const PhaseConditions &cond) {
    if (!SymbexOptions::get().crossPhaseDefaultActionPin) return;
    for (const auto &[tableName, rec] : cond.tableDefaultActionMap) {
        const auto *pinnable = annotationBackedSubset(tableName, rec.call);
        if (pinnable != nullptr) {
            init.addTestObject("pinned_default_actions"_cs, tableName, pinnable);
        }
    }
}

/// Per-argument half of the cross-phase check, shared by the keyless (default-action) and the keyed
/// (entry) comparison: the two calls are already known to be the same installed thing, so every
/// argument they both name has to carry the same value.
///
/// @p what names the thing in the diagnostic ("default action data" / "entry action data").
/// @returns false — after reporting — on the first argument BOTH paths branched on and disagree
/// about; true when every difference is vacuous.
static bool actionDataAgrees(const char *what, cstring tblName, const ActionCall *call1,
                             const std::set<cstring> &constrained1, const ActionCall *call2,
                             const std::set<cstring> &constrained2, size_t chainId,
                             cstring chainName, const char *phaseLabel) {
    const auto *args1 = call1->getArgs();
    const auto *args2 = call2->getArgs();
    if (args1 == nullptr || args2 == nullptr) return true;
    for (const auto &a1 : *args1) {
        const cstring argName = a1.getActionParamName();
        for (const auto &a2 : *args2) {
            if (a2.getActionParamName() != argName) continue;
            if (a1.getEvaluatedValue()->value == a2.getEvaluatedValue()->value) break;
            // Both paths branched on it, and they need different values.
            if (constrained1.count(argName) == 0 || constrained2.count(argName) == 0) break;
            printInfo("[Tampering] chain id=%1% (%2%): table '%3%' %4% %5%(%6%) must be %7% for "
                      "Phase 1 and %8% for %9%; the control plane installs one value for all "
                      "phases and no annotation fixes it, so this candidate is unrealizable — "
                      "skipping.",
                      chainId, chainName, tblName, what, call1->getActionName(), argName,
                      a1.getEvaluatedValue()->value, a2.getEvaluatedValue()->value, phaseLabel);
            return false;
        }
    }
    return true;
}

/// Cross-phase control-plane consistency CHECK — the destructive half of "annotation-backed ⇒ pin,
/// unconstrained ⇒ verify and drop".
///
/// Hardware installs ONE control-plane configuration for all three phases: one default action per
/// table, and one set of entries. An annotation-backed value is identical in every phase by
/// construction, so it never trips this check — the keyless path is pinned by
/// injectPinnedDefaultAction, and the keyed path needs no pin at all because TableStepper's
/// cpActionArgPin re-applies the very same clause inside every phase's own query. An UNCONSTRAINED
/// action-data symbol, by contrast, is a free variable in three independent solver queries: the
/// models legitimately disagree, and there is no sound way to elect a winner — so a disagreement
/// means the candidate needs a device configuration that cannot exist, and it is dropped.
///
/// Keyed entries are compared ENTRY-WISE, matched by their match values (sameInstalledEntry): two
/// entries the control plane can tell apart may carry whatever they like, and Phase 2's key is
/// often deliberately driven away from Phase 1's. Only entries that collapse onto one installed
/// entry have to agree.
///
/// A value the other phase never branched on is NOT a disagreement here: that phase is satisfiable
/// under any assignment to the symbol, so one installed value serves both. Only a difference that
/// BOTH paths constrain is contradictory. Likewise a table only one phase reached is not compared:
/// its installed configuration simply was not exercised there.
///
/// This is the early, cheap half of the check and it deliberately under-drops, because the models
/// compared here are not the ones that get written out: TestBackEnd::processPhase re-solves each
/// phase separately, and an unconstrained symbol can be re-rolled to a different value in that
/// re-solve. The emitted artifact is guaranteed self-consistent by emittedControlPlaneAgrees in
/// lib/test_backend.cpp, which compares the final specs with no such allowance. Dropping here saves
/// the emission work for candidates that are already provably contradictory.
static bool crossPhaseCpAgrees(
    const PhaseConditions &cond1,
    const std::vector<std::pair<const char *, const FinalState *>> &laterPhases, size_t chainId,
    cstring chainName) {
    if (!SymbexOptions::get().crossPhaseDefaultActionPin) return true;
    if (cond1.tableDefaultActionMap.empty() && cond1.tableEntryMap.empty()) return true;
    for (const auto &[phaseLabel, fs] : laterPhases) {
        if (fs == nullptr) continue;
        const auto other = collectCpState(*fs);
        for (const auto &[tblName, rec1] : cond1.tableDefaultActionMap) {
            auto it = other.defaults.find(tblName);
            if (it == other.defaults.end()) continue;
            const auto &rec2 = it->second;
            if (!cpNameMatches(rec1.call->getActionName(), rec2.call->getActionName())) {
                printInfo("[Tampering] chain id=%1% (%2%): %3% installs default action '%4%' on "
                          "table '%5%' while Phase 1 installs '%6%'; one device cannot hold both, "
                          "so this candidate is unrealizable — skipping.",
                          chainId, chainName, phaseLabel, rec2.call->getActionName(), tblName,
                          rec1.call->getActionName());
                return false;
            }
            if (!actionDataAgrees("default action data", tblName, rec1.call, rec1.constrainedArgs,
                                  rec2.call, rec2.constrainedArgs, chainId, chainName,
                                  phaseLabel)) {
                return false;
            }
        }
        for (const auto &[tblName, entries1] : cond1.tableEntryMap) {
            auto it = other.entries.find(tblName);
            if (it == other.entries.end()) continue;
            for (const auto &e1 : entries1) {
                for (const auto &e2 : it->second) {
                    // Not the same installed entry: two entries the control plane distinguishes,
                    // which may hold different actions and different data. Nothing to check.
                    if (!sameInstalledEntry(e1, e2)) continue;
                    if (e1.call == nullptr || e2.call == nullptr) continue;
                    if (!cpNameMatches(e1.call->getActionName(), e2.call->getActionName())) {
                        printInfo("[Tampering] chain id=%1% (%2%): %3% installs action '%4%' on "
                                  "table '%5%' behind the same match key that Phase 1 gives "
                                  "'%6%'; one entry cannot run two actions, so this candidate is "
                                  "unrealizable — skipping.",
                                  chainId, chainName, phaseLabel, e2.call->getActionName(), tblName,
                                  e1.call->getActionName());
                        return false;
                    }
                    if (!actionDataAgrees("entry action data", tblName, e1.call, e1.constrainedArgs,
                                          e2.call, e2.constrainedArgs, chainId, chainName,
                                          phaseLabel)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/// The tampering serializers merge table RULES only, so a default-action override — pinned across
/// the phases by the two helpers above — still never reaches the emitted `entities`. Emitting it
/// needs a proto/harness change that is deliberately deferred, so report the gap once per emitted
/// test: a silent omission reads as "this table did not matter", which is the opposite of true.
static void reportUnserializedDefaultActions(const PhaseConditions &cond, size_t chainId,
                                             size_t subTestId) {
    if (cond.tableDefaultActionMap.empty()) return;
    std::stringstream list;
    bool isFirst = true;
    for (const auto &[tableName, rec] : cond.tableDefaultActionMap) {
        if (!isFirst) list << ", ";
        isFirst = false;
        list << tableName << ": " << rec.call->getActionName() << "(";
        const auto *args = rec.call->getArgs();
        if (args != nullptr) {
            bool isFirstArg = true;
            for (const auto &arg : *args) {
                if (!isFirstArg) list << ", ";
                isFirstArg = false;
                list << arg.getActionParamName() << "=" << arg.getEvaluatedValue()->value;
            }
        }
        list << ")";
    }
    printInfo("[Tampering] chain id=%1% sub=%2%: default-action override(s) NOT serialized into the "
              "test case (the emitters merge table rules only) — install out of band: %3%",
              chainId, subTestId, list.str());
}

// ---------------------------------------------------------------------------
// Sink action-divergence (replaces the cross-phase Phase1↔Phase3 output diff)
// ---------------------------------------------------------------------------
// A sink-table HIT↔MISS flip is only observable if the table's HIT action and its default (MISS)
// action write *different* output state. We compare the two action bodies *locally* — no
// continuation, no terminal, no downstream result consulted. The real switch (differential oracle)
// decides the end-to-end effect; this gate only drops flips that are provably invisible.
// Sound-toward-emitting: anything we cannot prove identical counts as divergent (emit).
namespace {

// ---------------------------------------------------------------------------
// Class-A (impact) chain ranking — --chain-impact-order
//
// A chain is class A when its sink can reach an ENFORCEMENT PRIMITIVE: something that changes the
// packet's fate (drop / forward / replicate), as opposed to only setting metadata. Ordering-only: it
// decides WHICH chains a truncated run processes, never whether a test is accepted or emitted.
// ---------------------------------------------------------------------------

/// Intrinsic/standard-metadata fields whose write decides the packet's fate (v1model + TNA).
bool isEnforcementFieldName(cstring n) {
    return n == "drop_ctl" || n == "egress_spec" || n == "ucast_egress_port" || n == "egress_port" ||
           n == "mcast_grp" || n == "mcast_grp_a" || n == "mcast_grp_b" || n == "multicast_group_id" ||
           n == "mirror_type" || n == "copy_to_cpu";
}

/// Externs that drop/replicate/exfiltrate a packet outright.
bool isEnforcementMethodName(cstring n) {
    return n == "mark_to_drop" || n == "clone" || n == "clone3" ||
           n == "clone_preserving_field_list" || n == "digest" || n == "mirror" ||
           n == "mirror_packet";
}

/// Trailing component of a method call's name (`X.apply` -> "apply", `mark_to_drop` -> itself).
cstring methodTailName(const IR::MethodCallExpression *mce) {
    if (const auto *m = mce->method->to<IR::Member>()) return m->member.name;
    if (const auto *p = mce->method->to<IR::PathExpression>()) return p->path->name.name;
    return ""_cs;
}

/// Name of the object a `.apply()` is called on (`drop_tbl.apply()` -> "drop_tbl").
cstring applyTargetName(const IR::MethodCallExpression *mce) {
    const auto *m = mce->method->to<IR::Member>();
    if (m == nullptr) return ""_cs;
    if (const auto *p = m->expr->to<IR::PathExpression>()) return p->path->name.name;
    if (const auto *mm = m->expr->to<IR::Member>()) return mm->member.name;
    return ""_cs;
}

/// One pass over a statement subtree: does it enforce directly, which tables does it apply, and
/// which non-enforcement fields does it assign (candidate metadata flags for the transitive step)?
struct SinkScan : public Inspector {
    bool enforce = false;
    std::set<cstring> appliedTables;
    std::set<cstring> assignedFields;

    bool preorder(const IR::AssignmentStatement *a) override {
        if (const auto *m = a->left->to<IR::Member>()) {
            if (isEnforcementFieldName(m->member.name)) {
                enforce = true;
            } else {
                assignedFields.insert(m->member.name);
            }
        }
        return true;
    }
    bool preorder(const IR::MethodCallExpression *mce) override {
        auto nm = methodTailName(mce);
        if (isEnforcementMethodName(nm)) enforce = true;
        if (nm == "apply") {
            auto t = applyTargetName(mce);
            if (!t.isNullOrEmpty()) appliedTables.insert(t);
        }
        return true;
    }
};

/// Last component of a dotted name with the midend's `_<n>` instantiation suffix removed, so an
/// apply site's `drop_tbl_0` matches the control-plane name `Ingress.drop_tbl`.
std::string normalizedTail(cstring n) {
    std::string s(n.string_view());
    auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(dot + 1);
    auto us = s.find_last_of('_');
    if (us != std::string::npos && us + 1 < s.size() &&
        std::all_of(s.begin() + static_cast<ptrdiff_t>(us) + 1, s.end(),
                    [](char c) { return c >= '0' && c <= '9'; }))
        s = s.substr(0, us);
    return s;
}

/// Resolve a name written at a call site (`drop_tbl_0`) against a control-plane-name map
/// (`Ingress.drop_tbl`). Exact match first, then dotted-suffix, then normalized tail.
template <typename T>
const T *lookupByCallSiteName(const std::unordered_map<cstring, const T *> &decls, cstring bare) {
    auto exact = decls.find(bare);
    if (exact != decls.end()) return exact->second;
    const std::string suffix = "." + std::string(bare.string_view());
    for (const auto &[nm, d] : decls) {
        const std::string s(nm.string_view());
        if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            return d;
    }
    const std::string tail = normalizedTail(bare);
    if (tail.empty()) return nullptr;
    for (const auto &[nm, d] : decls)
        if (normalizedTail(nm) == tail) return d;
    return nullptr;
}

const IR::P4Table *lookupTableBySuffix(
    const std::unordered_map<cstring, const IR::P4Table *> &tables, cstring bare) {
    return lookupByCallSiteName(tables, bare);
}

/// Every IR::P4Action in the program, keyed by both its bare and control-plane name, so an action
/// referenced from a table's action list can be resolved to its BODY (an ActionListElement only
/// names the action).
struct ActionCollector : public Inspector {
    std::unordered_map<cstring, const IR::P4Action *> &out;
    explicit ActionCollector(std::unordered_map<cstring, const IR::P4Action *> &o) : out(o) {}
    bool preorder(const IR::P4Action *a) override {
        out.emplace(a->name.name, a);
        out.emplace(a->controlPlaneName(), a);
        return true;
    }
};

const IR::P4Action *lookupActionBySuffix(
    const std::unordered_map<cstring, const IR::P4Action *> &actions, cstring bare) {
    return lookupByCallSiteName(actions, bare);
}

/// Scan a subtree, following applied tables into their ACTION BODIES (bounded depth: an action may
/// apply another table). Accumulates assigned fields for the transitive metadata step.
void scanWithTables(const IR::Node *root,
                    const std::unordered_map<cstring, const IR::P4Table *> &tables,
                    const std::unordered_map<cstring, const IR::P4Action *> &actions, int depth,
                    bool &enforce, std::set<cstring> &assignedFields) {
    if (root == nullptr || enforce || depth < 0) return;
    SinkScan scan;
    root->apply(scan);
    if (scan.enforce) {
        enforce = true;
        return;
    }
    assignedFields.insert(scan.assignedFields.begin(), scan.assignedFields.end());
    for (const auto &tname : scan.appliedTables) {
        const auto *tbl = lookupTableBySuffix(tables, tname);
        if (tbl == nullptr) continue;
        // The table's actions decide the fate; scan every action body it may run, plus its default.
        const auto *al = tbl->getActionList();
        if (al == nullptr) continue;
        for (const auto *ale : al->actionList) {
            const auto *mce = ale->expression->to<IR::MethodCallExpression>();
            cstring actName;
            if (mce != nullptr) {
                actName = methodTailName(mce);
            } else if (const auto *pe = ale->expression->to<IR::PathExpression>()) {
                actName = pe->path->name.name;
            }
            if (actName.isNullOrEmpty()) continue;
            const auto *act = lookupActionBySuffix(actions, actName);
            if (act == nullptr) continue;
            scanWithTables(act->body, tables, actions, depth - 1, enforce, assignedFields);
            if (enforce) return;
        }
    }
}

/// True when @p chain's sink can reach an enforcement primitive — directly, through the actions of
/// an applied table, or transitively through a metadata flag the sink branch sets that later gates
/// one. Sound toward NOT ranking: an unresolvable sink simply is not class A (ordering only).
bool chainIsClassA(const P4StateDependency::DependencyGraphs::SOChain &chain,
                   const IR::P4Program &program,
                   const std::unordered_map<cstring, const IR::P4Table *> &tables,
                   const std::unordered_map<cstring, const IR::P4Action *> &actions) {
    bool enforce = false;
    std::set<cstring> flags;

    if (chain.sinkConditionNode != nullptr) {
        // Condition sink: the branches the flip chooses between.
        if (const auto *ifs = chain.sinkConditionNode->to<IR::IfStatement>()) {
            scanWithTables(ifs->ifTrue, tables, actions, 3, enforce, flags);
            scanWithTables(ifs->ifFalse, tables, actions, 3, enforce, flags);
        }
    } else if (!chain.sinkTableControlPlaneName.isNullOrEmpty()) {
        // Table sink: the HIT/MISS actions the tampered key chooses between.
        const auto *tbl = lookupTableBySuffix(tables, chain.sinkTableControlPlaneName);
        if (tbl != nullptr) {
            // Re-use the table-following scan by handing it the table's own apply site.
            std::set<cstring> tnames;
            const auto *al = tbl->getActionList();
            if (al != nullptr) {
                for (const auto *ale : al->actionList) {
                    const auto *mce = ale->expression->to<IR::MethodCallExpression>();
                    cstring actName;
                    if (mce != nullptr) {
                        actName = methodTailName(mce);
                    } else if (const auto *pe = ale->expression->to<IR::PathExpression>()) {
                        actName = pe->path->name.name;
                    }
                    if (actName.isNullOrEmpty()) continue;
                    const auto *act = lookupActionBySuffix(actions, actName);
                    if (act == nullptr) continue;
                    scanWithTables(act->body, tables, actions, 3, enforce, flags);
                    if (enforce) return true;
                }
            }
        }
    }
    if (enforce) return true;
    if (flags.empty()) return false;

    // Transitive: a metadata flag set by the sink branch that later gates an enforcement primitive.
    struct FlagGateFinder : public Inspector {
        const std::set<cstring> &flags;
        const std::unordered_map<cstring, const IR::P4Table *> &tables;
        const std::unordered_map<cstring, const IR::P4Action *> &actions;
        bool found = false;
        FlagGateFinder(const std::set<cstring> &f,
                       const std::unordered_map<cstring, const IR::P4Table *> &t,
                       const std::unordered_map<cstring, const IR::P4Action *> &a)
            : flags(f), tables(t), actions(a) {}
        bool preorder(const IR::IfStatement *ifs) override {
            if (found) return false;
            // Does this condition mention one of the flags?
            struct MentionsFlag : public Inspector {
                const std::set<cstring> &flags;
                bool hit = false;
                explicit MentionsFlag(const std::set<cstring> &f) : flags(f) {}
                bool preorder(const IR::Member *m) override {
                    if (flags.count(m->member.name) > 0) hit = true;
                    return true;
                }
            } mf(flags);
            ifs->condition->apply(mf);
            if (!mf.hit) return true;
            bool enf = false;
            std::set<cstring> ignored;
            scanWithTables(ifs->ifTrue, tables, actions, 3, enf, ignored);
            scanWithTables(ifs->ifFalse, tables, actions, 3, enf, ignored);
            if (enf) found = true;
            return true;
        }
    } fg(flags, tables, actions);
    program.apply(fg);
    return fg.found;
}

}  // namespace

bool StateDependencyTracker::sinkActionsDiverge(const FinalState *fs, const IR::P4Table *sink,
                                                cstring sinkCpName) const {
    // No resolvable sink -> cannot prove invisibility -> emit.
    if (sink == nullptr || fs == nullptr) return true;
    const auto *es = fs->getExecutionState();

    // --- Default (MISS) action and its bound (const) args ---
    const auto *defActExpr = sink->getDefaultAction();
    if (defActExpr == nullptr) return true;
    const auto *defMce = defActExpr->to<IR::MethodCallExpression>();
    if (defMce == nullptr) return true;
    const auto *defAction = es->getP4Action(defMce);
    if (defAction == nullptr) return true;
    std::map<cstring, const IR::Expression *> defBinding;
    {
        const auto &params = defAction->parameters->parameters;
        const auto *args = defMce->arguments;
        if (args != nullptr && params.size() == args->size())
            for (size_t i = 0; i < params.size(); ++i)
                defBinding[params.at(i)->name.name] = args->at(i)->expression;
    }

    // --- HIT action: the action chosen by the sink's matched entry in this terminal ---
    const auto *tblObj = es->getTestObject("tableconfigs"_cs, sinkCpName, /*checked=*/false);
    if (tblObj == nullptr) return true;
    const auto *cfg = tblObj->evaluate(fs->getFinalModel(), /*doComplete=*/true)->to<TableConfig>();
    if (cfg == nullptr || cfg->getRules() == nullptr || cfg->getRules()->empty()) return true;
    const auto *hitCall = cfg->getRules()->front().getActionCall();
    if (hitCall == nullptr || hitCall->getAction() == nullptr) return true;
    std::map<cstring, const IR::Expression *> hitBinding;
    for (const auto &arg : *hitCall->getArgs())
        hitBinding[arg.getActionParamName()] = arg.getEvaluatedValue();

    // Diverge unless the two action bodies are provably identical in observable effect. The
    // comparison itself lives in sink_divergence, shared with the condition and const-entry sinks.
    return outcomesDiverge(hitCall->getAction(), hitBinding, defAction, defBinding);
}

bool StateDependencyTracker::sinkConditionDiverges() const {
    // H2S2C analog of sinkActionsDiverge: a condition flip is observable only if the then-branch and
    // else-branch write different output state. Same comparison as the table sink, applied to the
    // two branch bodies instead of two action bodies.
    return branchesDiverge(currentSinkCondition);
}

StateDependencyTracker::StateDependencyTracker(
    AbstractSolver &solver, const ProgramInfo &programInfo,
    const P4StateDependency::StateDependencyResult &sdResult, StateDependencyPolicy policy)
    : SymbolicExecutor(solver, programInfo), sdResult(sdResult), policy(policy) {}

std::map<cstring, std::vector<const P4StateDependency::DependencyGraphs::SOChain *>>
StateDependencyTracker::collectChains() const {
    std::map<cstring, std::vector<const P4StateDependency::DependencyGraphs::SOChain *>> result;

    auto addChains = [&](const std::map<cstring, std::vector<P4StateDependency::DependencyGraphs::SOChain>> &chainMap,
                        cstring chainName = ""_cs) {
        for (const auto &[graphName, chains] : chainMap)
            for (const auto &chain : chains)
                result[chainName].push_back(&chain);
    };

    switch (policy) {
        case StateDependencyPolicy::Tampering:
            addChains(sdResult.dataWriteKeyChains, "Write Key"_cs);
            break;
        case StateDependencyPolicy::TamperingCond:
            addChains(sdResult.dataWriteCondChains, "Write Condition"_cs);
            break;
    }
    return result;
}

P4::Coverage::CoverageSet StateDependencyTracker::buildRequiredNodes(
    const P4StateDependency::DependencyGraphs::SOChain &chain) const {
    P4::Coverage::CoverageSet nodes;
    switch (policy) {
        case StateDependencyPolicy::Tampering:
        case StateDependencyPolicy::TamperingCond:
            // Phase 2 targets write nodes; Phase 1 and Phase 3 target read nodes (same staging for
            // Key and Condition sinks — only the flip check differs, downstream).
            if (currentPhase == TamperingPhase::Phase2_Write) {
                for (const auto &[v, node] : chain.writeNodes)
                    if (node != nullptr) nodes.insert(node);
            } else if (chain.isUpdate) {
                // Update chains (read-modify-write in one action) have no separate read path:
                // the register read lives inside the update (writeNodes), and readNodes is empty.
                // For these, the read phase (Phase 1 baseline / Phase 3 replay) targets the
                // update's writeNodes so Phase 1 runs the update from clean state and the sink
                // HITs on the written value; the directed search + sink-HIT steering then apply.
                for (const auto &[v, node] : chain.writeNodes)
                    if (node != nullptr) nodes.insert(node);
            } else {
                for (const auto &[v, node] : chain.readNodes)
                    if (node != nullptr) nodes.insert(node);
            }
            break;
    }
    return nodes;
}

// ---------------------------------------------------------------------------
// Key → Table mapping helpers
// ---------------------------------------------------------------------------

void StateDependencyTracker::buildTableByNameMap() {
    // Walk every P4Table and record controlPlaneName() → IR::P4Table*.
    struct Collector : Inspector {
        std::unordered_map<cstring, const IR::P4Table *> &out;
        explicit Collector(std::unordered_map<cstring, const IR::P4Table *> &o) : out(o) {}
        bool preorder(const IR::P4Table *tbl) override {
            out.emplace(tbl->controlPlaneName(), tbl);
            return true;
        }
    } col(tableByName_);
    programInfo.getP4Program().apply(col);
}

void StateDependencyTracker::buildRegisterActionBodyNodes() {
    // Record the SOURCE POSITION of every node inside an abstract-method body (IR::Function) — i.e.
    // a RegisterAction's `apply`. Positions (not pointers) because the chain's required nodes come
    // from the SD dependency graph's IR, whose pointers differ from this program's. A node
    // read/written by an SD chain can only be inside a Function if it is the SO's RegisterAction
    // body, so over-collecting other Functions is harmless: only required nodes are looked up. The
    // `.execute()` call site and the sink key live in the control body (different positions), so they
    // are never excused.
    struct Collector : Inspector {
        std::unordered_set<cstring> &out;
        int depth = 0;
        explicit Collector(std::unordered_set<cstring> &o) : out(o) {}
        bool preorder(const IR::Function *fn) override {
            out.insert(cstring(fn->getSourceInfo().toPositionString()));
            ++depth;
            return true;
        }
        void postorder(const IR::Function *) override { --depth; }
        bool preorder(const IR::Node *n) override {
            if (depth > 0) out.insert(cstring(n->getSourceInfo().toPositionString()));
            return true;
        }
    } col(registerActionBodyPositions_);
    programInfo.getP4Program().apply(col);
}

bool StateDependencyTracker::isInRegisterActionBody(const IR::Node *n) const {
    return registerActionBodyPositions_.count(cstring(n->getSourceInfo().toPositionString())) > 0;
}

bool StateDependencyTracker::isTableVisited(
        cstring controlPlaneName, const P4::Coverage::CoverageSet &visited) const {
    auto it = tableByName_.find(controlPlaneName);
    if (it == tableByName_.end()) return false;
    // TableStepper::eval() calls markVisited(table) so a direct pointer lookup suffices.
    return visited.count(it->second) > 0;
}

// ---------------------------------------------------------------------------
// Three-phase tampering entry point
// ---------------------------------------------------------------------------

namespace {
/// Defined below with the rest of the --dump-cp-stubs report; declared here because the chain loop
/// that populates and flushes it comes first in this file.
void writeCpStubs();
void recordSinkStub(const IR::P4Table *table, cstring cpName, const IR::P4Program *program,
                    bool isSink);
ActionResolver actionResolverFor(const IR::P4Program *program);
}  // namespace

void StateDependencyTracker::runTampering(const TamperingCallback &callBack) {
    auto chains = collectChains();
    if (chains.empty()) {
        warning("State-dependency analysis produced no chains for the selected policy.");
        return;
    }
    auto &initState = ExecutionState::create(&programInfo.getP4Program());
    runTamperingScenario(callBack, initState);
}

// RAII helper — saves/restores SymbexOptions fields around a phase run.
// If sinkTableName is non-empty, temporarily adds it to skippedControlPlaneEntities
// so the table produces no synthesized entries (only its default action) during this phase.
// evalTableConstEntries() returns "always-miss" (true) when the table has no constant
// entries, so addDefaultAction takes the default unconditionally — no entry is emitted.
// extraSkippedTables lists additional tables to suppress entry generation for (e.g.
// size-1 tables whose single slot was already consumed by Phase 1).
struct ScopedSymbexOpts {
    bool savedOutputPacketOnly;
    // Forced true during SD execution so that IR::AssignmentStatement nodes
    // (e.g. inside RegisterAction::apply) are added to visitedNodes.
    bool savedCoverStatements;
    // Forced true so RegisterAction.execute() always creates a symbolic
    // TofinoRegisterValue test object and emits tofino_register_writeback for both
    // Phase 1 and Phase 2 (register reads in Phase 1 also update state).
    bool savedTamperingRegisterTracking;
    bool savedInitRegZeroValue;
    bool savedRelaxCarriedRegisterRead;
    cstring sinkTableName_ = ""_cs;
    std::vector<cstring> extraSkippedTables_;
    ScopedSymbexOpts(bool setOutputPacketOnly, cstring sinkTableName = ""_cs,
                     std::vector<cstring> extraSkippedTables = {},
                     bool setRegTracking = true,
                     bool isPhase1 = false,
                     bool relaxCarriedRead = false)
        : extraSkippedTables_(std::move(extraSkippedTables)) {
        auto &opts = SymbexOptions::get();
        savedOutputPacketOnly             = opts.outputPacketOnly;
        savedCoverStatements              = opts.coverageOptions.coverStatements;
        savedTamperingRegisterTracking    = opts.tamperingRegisterTracking;
        savedInitRegZeroValue        = opts.initRegZeroValue;
        savedRelaxCarriedRegisterRead     = opts.relaxCarriedRegisterRead;
        opts.outputPacketOnly             = setOutputPacketOnly;
        opts.coverageOptions.coverStatements = true;
        opts.tamperingRegisterTracking    = setRegTracking;
        opts.initRegZeroValue        = isPhase1;
        opts.relaxCarriedRegisterRead     = relaxCarriedRead;
        sinkTableName_ = sinkTableName;
        if (!sinkTableName_.isNullOrEmpty()) {
            opts.skippedControlPlaneEntities.insert(sinkTableName_);
        }
        for (const auto &n : extraSkippedTables_) {
            opts.skippedControlPlaneEntities.insert(n);
        }
    }
    ~ScopedSymbexOpts() {
        auto &opts = SymbexOptions::get();
        opts.outputPacketOnly             = savedOutputPacketOnly;
        opts.coverageOptions.coverStatements     = savedCoverStatements;
        opts.tamperingRegisterTracking    = savedTamperingRegisterTracking;
        opts.initRegZeroValue        = savedInitRegZeroValue;
        opts.relaxCarriedRegisterRead     = savedRelaxCarriedRegisterRead;
        if (!sinkTableName_.isNullOrEmpty()) {
            opts.skippedControlPlaneEntities.erase(sinkTableName_);
        }
        for (const auto &n : extraSkippedTables_) {
            opts.skippedControlPlaneEntities.erase(n);
        }
    }
};

void StateDependencyTracker::runTamperingScenario(const TamperingCallback &callBack,
                                                   const ExecutionState &initState) {
    // Build the controlPlanName → IR::P4Table* map once for this execution so that
    // allCovered can check whether a table's apply() was visited in visitedNodes.
    buildTableByNameMap();
    // RegisterAction-body node set: scopes the read/write coverage relaxation to in-RegisterAction
    // blocks only (the mutually-exclusive branch siblings); built once, like the table map.
    buildRegisterActionBodyNodes();

    auto chains = collectChains();

    // Flatten all chains for the shared Phase-1 pass.
    allChains.clear();
    for (const auto &[chainName, chainList] : chains)
        for (const auto *chain : chainList) allChains.push_back(chain);
    if (allChains.empty()) return;

    // One shared Phase-1 traversal of the whole program collects read-baseline terminals for EVERY
    // chain at once (bucketed per chain), instead of re-traversing the program once per chain.
    // --shared-traversal=NONE selects the per-chain baseline instead (collected inside the loop).
    const bool sharedPhase1Pass =
        SymbexOptions::get().sharedTraversal != SharedTraversalMode::None;
    if (sharedPhase1Pass) collectPhase1Terminals(initState);

    // --shared-traversal=PHASE1_PHASE2: a global write-path prefilter prunes chains whose write is
    // unreachable BEFORE the expensive per-chain Phase-2/3 loop (empty write bucket => prune).
    std::set<size_t> prunedChains;
    if (SymbexOptions::get().sharedTraversal == SharedTraversalMode::Phase1Phase2)
        prunedChains = collectPhase2Terminals(initState);

    // --chain-impact-order: rank chains whose sink reaches an enforcement primitive (class A) ahead
    // of metadata-only sinks. ORDERING ONLY — a truncated run then spends its budget on the chains
    // whose findings matter, instead of whichever ids happened to come first.
    std::set<size_t> classAChains;
    if (SymbexOptions::get().chainImpactOrder) {
        std::unordered_map<cstring, const IR::P4Action *> actionByName;
        ActionCollector ac(actionByName);
        programInfo.getP4Program().apply(ac);
        for (const auto *chain : allChains)
            if (chainIsClassA(*chain, programInfo.getP4Program(), tableByName_, actionByName))
                classAChains.insert(chain->id);
        printInfo("[Tampering] Chain order: %1% impact-ranked first of %2%", classAChains.size(),
                  allChains.size());
    }

    // Per-chain cap on emitted sub-tests, reusing the existing --max-tests option. Applied per chain
    // (not globally) so every SOChain produces its own tests. 0 means "unlimited".
    const size_t maxPerChain = static_cast<size_t>(SymbexOptions::get().maxTests);
    size_t chainsCompleted = 0;
    for (const auto &[chainName, chainList] : chains) {
        // Stable partition keeps chain-id order WITHIN each class, so the only difference from the
        // default schedule is that class-A chains come first (and none when the option is off).
        std::vector<const P4StateDependency::DependencyGraphs::SOChain *> ordered(chainList.begin(),
                                                                                 chainList.end());
        if (SymbexOptions::get().chainImpactOrder)
            std::stable_partition(ordered.begin(), ordered.end(),
                                  [&classAChains](const auto *c) { return classAChains.count(c->id) > 0; });
        for (const auto *chain : ordered) {
            if (prunedChains.count(chain->id)) {  // write path unreachable -> Phase-2 prefilter pruned
                printInfo("============ Chain (%1%) id=%2% %3% [Phase-2 prefilter: PRUNED] ============",
                          chainName, chain->id, chain->soName);
                ++chainsCompleted;
                continue;
            }
            currentChain = chain;
            currentChainName = chainName;
            printInfo("============ Chain (%1%) id=%2% %3% [Tampering 3-phase] ============",
                      chainName, chain->id, chain->soName);
            // Held by value in the NONE case; a reference would dangle past the if/else.
            std::vector<const FinalState *> perChainBucket;
            if (!sharedPhase1Pass) perChainBucket = collectPhase1TerminalsPerChain(*chain, initState);
            const auto &bucket = sharedPhase1Pass ? phase1Buckets[chain->id] : perChainBucket;
            currentChain = chain;  // collectPhase1TerminalsPerChain clears it
            currentChainName = chainName;
            runTamperingChain(*chain, initState, bucket, callBack, maxPerChain, /*missToHit=*/false);
            runTamperingChain(*chain, initState, bucket, callBack, maxPerChain, /*missToHit=*/true);
            ++chainsCompleted;
        }
    }
    // Any table with an action PARAMETER is annotatable, sink or not -- SketchLib's threshold lives
    // in tbl_get_threshold, which is upstream of the sink and would otherwise never be reported even
    // though it is the table that actually needed an assume clause.
    if (SymbexOptions::get().cpStubsPath.has_value()) {
        for (const auto &[cpName, table] : tableByName_) {
            bool hasActionData = false;
            const auto resolve = actionResolverFor(&programInfo.getP4Program());
            if (const auto *al = table->getActionList()) {
                for (const auto *ale : al->actionList) {
                    const auto *mce = ale->expression->to<IR::MethodCallExpression>();
                    const auto *pe = mce != nullptr ? mce->method->to<IR::PathExpression>() : nullptr;
                    const auto *act = pe != nullptr ? resolve(pe->path->name.name) : nullptr;
                    if (act != nullptr && !act->parameters->parameters.empty()) hasActionData = true;
                }
            }
            if (hasActionData) recordSinkStub(table, cpName, &programInfo.getP4Program(), false);
        }
    }

    // Headline progress metric: on a run that is cut short by a timeout this is the only record of how
    // far the per-chain Phase-2/3 loop actually got. Emitted in every mode so baselines are comparable.
    printInfo("[Tampering] Chains completed: %1%/%2%", chainsCompleted, allChains.size());
    writeCpStubs();
}

// ---------------------------------------------------------------------------
// Shared Phase-1 collection: one traversal, terminals bucketed per chain
// ---------------------------------------------------------------------------

bool StateDependencyTracker::chainTargetsCovered(
    const P4StateDependency::DependencyGraphs::SOChain &chain,
    const P4::Coverage::CoverageSet &visited, bool soRan) const {
    auto it = chainPhase1Targets.find(chain.id);
    if (it == chainPhase1Targets.end() || it->second.empty()) return false;
    return std::all_of(it->second.begin(), it->second.end(),
                       [&visited, &chain, soRan, this](const IR::Node *n) {
                           if (n->is<IR::Key>())
                               return isTableVisited(chain.sinkTableControlPlaneName, visited);
                           if (visited.count(n) > 0) return true;
                           // In-RegisterAction node: excused once the SO's RegisterAction ran, since
                           // it may be a mutually-exclusive branch sibling no path can also cover.
                           // Control-body nodes (incl. the .execute() call site) are not in the set,
                           // so they stay strictly required.
                           return soRan && isInRegisterActionBody(n);
                       });
}

bool StateDependencyTracker::conditionReached(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &es) const {
    if (chain.sinkConditionNode == nullptr) return false;
    const auto *ifStmt = chain.sinkConditionNode->to<IR::IfStatement>();
    if (ifStmt == nullptr) return false;
    // The branch-stamped condition var is set iff the if-statement was reached on this path.
    // exists() (not get()) so an unreached if returns false instead of BUGging on the missing var.
    return es.exists(CmdStepper::getConditionVar(ifStmt));
}

bool StateDependencyTracker::allPhase1BucketsFull() const {
    for (const auto *ch : allChains) {
        auto it = phase1Buckets.find(ch->id);
        if (it == phase1Buckets.end() || it->second.size() < phase1BucketCap) return false;
    }
    return true;
}

// Set by collectPhase1TerminalsPerChain (--shared-traversal=NONE) to restrict handleSharedTerminal's
// bucketing to a single chain, so the per-chain baseline reuses the shared bucketing predicate
// (conditionReached / chainTargetsCovered) rather than runPhase's allCovered acceptance — the latter
// rejects every RMW read terminal (the threshold branch is unreachable in one packet). File-local
// because both the setter and the sole reader live in this translation unit.
static const P4StateDependency::DependencyGraphs::SOChain *phase1SingleChain = nullptr;

void StateDependencyTracker::handleSharedTerminal(const ExecutionState &es) {
    ++phase1Examined;
    const auto &visited = es.getVisited();
    std::vector<size_t> matched;
    for (const auto *ch : allChains) {
        if (phase1SingleChain != nullptr && ch != phase1SingleChain) continue;  // NONE: per-chain scope
        if (phase1Buckets[ch->id].size() >= phase1BucketCap) continue;  // bucket already full
        // Condition chains (H2S2C): bucket iff the if-condition was reached (baseline value exists).
        // Key chains: require read-node coverage. chainTargetsCovered excuses in-RegisterAction read
        // nodes once the SO's RegisterAction ran (soRan): a read-modify-write action's body branches
        // (e.g. count-sketch `if(res==0) data-1 else data+1`) are mutually exclusive, so no single
        // path covers them all. The .execute() call site and the sink key stay strictly required.
        bool covered;
        if (ch->sinkConditionNode != nullptr) {
            covered = conditionReached(*ch, es);
        } else {
            const bool soRan =
                es.getTestObject("registervalues"_cs, ch->soName, /*checked=*/false) != nullptr;
            covered = chainTargetsCovered(*ch, visited, soRan);
        }
        if (covered) matched.push_back(ch->id);
    }
    if (matched.empty()) return;
    auto sat = solver.checkSat(es.getPathConstraint());
    if (!sat || !*sat) return;
    // One materialized terminal is shared (read-only) across all chains it covers.
    const auto *fs = new FinalState(solver, es);
    for (auto id : matched) phase1Buckets[id].push_back(fs);
}

void StateDependencyTracker::collectPhase1Terminals(const ExecutionState &initState) {
    phase1Buckets.clear();
    chainPhase1Targets.clear();
    phase1Examined = 0;

    // Per-chain Phase-1 targets (readNodes, or writeNodes for isUpdate) and their union.
    currentPhase = TamperingPhase::Phase1_Read;
    P4::Coverage::CoverageSet unionTargets;
    for (const auto *chain : allChains) {
        currentChain = chain;  // buildRequiredNodes consults currentPhase + chain
        auto targets = buildRequiredNodes(*chain);
        // For condition chains also steer toward the if-statement itself, so the DFS heads to where
        // the baseline condition value is observed (read-modify-write SOs have empty readNodes).
        if (chain->sinkConditionNode != nullptr) targets.insert(chain->sinkConditionNode);
        for (const auto *n : targets) unionTargets.insert(n);
        chainPhase1Targets[chain->id] = std::move(targets);
    }
    currentChain = nullptr;
    if (unionTargets.empty()) return;

    // Caps. The bucket serves BOTH directions (HIT→MISS keeps sink-HIT terminals, MISS→HIT keeps
    // sink-MISS); since the shared pass has no per-chain sink steering, collect generously so both
    // subsets have material. The examine budget bounds runaway exploration when a chain's target is
    // single-packet-infeasible (its bucket never fills). Both are tunable.
    const size_t base = std::max<size_t>(static_cast<size_t>(SymbexOptions::get().maxTests), 1);
    phase1BucketCap = std::max<size_t>(base * 4, 12);
    phase1ExamineBudget = std::max<size_t>(phase1BucketCap * allChains.size() * 4, 2000);

    // Multi-target directed search: steer toward / prune via the UNION of all chains' read targets.
    currentRequiredNodes = unionTargets;
    // Condition chains are bucketed by "the if-statement was reached", which requires the path to
    // continue PAST the condition to a terminal (so the branch-stamped condition var survives).
    // Reaching-set pruning cuts the path right after a target, so disable it for the condition
    // policy — keep the (non-pruning) steering toward required nodes only.
    if (policy == StateDependencyPolicy::TamperingCond) {
        reachingSet_.clear();
        reachingSetValid_ = false;
    } else {
        buildReachingSet();
    }

    solver.checkSat({});  // clear accumulated assertions before the read pass
    sharedPhase1 = true;
    seekMiss_ = false;
    currentSinkTable_ = nullptr;
    {
        // Allow drops (serves both directions) + register zero-init + register tracking.
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, {}, /*setRegTracking=*/true,
                               /*isPhase1=*/true);
        unexploredBranches.clear();
        auto &phase1Init = initState.clone();
        // No-op callback: shared bucketing happens in runImpl/handleSharedTerminal, not the callback.
        runImpl([](const FinalState &) -> bool { return false; }, phase1Init);
    }
    sharedPhase1 = false;
    currentChain = nullptr;

    size_t total = 0;
    for (const auto &[id, b] : phase1Buckets) total += b.size();
    printInfo("[Tampering] Shared Phase-1: %1% chains, %2% terminals examined, %3% bucketed "
              "(cap %4%/chain)",
              allChains.size(), phase1Examined, total, phase1BucketCap);
}

// ---------------------------------------------------------------------------
// Shared Phase-2 write-path prefilter (--shared-traversal=PHASE1_PHASE2)
// ---------------------------------------------------------------------------

// Set by runImpl when the prefilter DFS stops on its examine budget rather than exploring the write
// paths to exhaustion. In that case reachability is UNDETERMINED for the chains not yet reached, so
// collectPhase2Terminals must prune nothing: "not reached within budget" != "unreachable", and
// pruning on it would silently drop real tests on exactly the large programs this is meant to help.
// File-local because the setter (runImpl) and the sole reader (collectPhase2Terminals) share this TU.
static bool phase2BudgetExhausted = false;

bool StateDependencyTracker::allPhase2BucketsFull() const {
    return phase2Reached.size() >= allChains.size();
}

void StateDependencyTracker::handleSharedPhase2Terminal(const ExecutionState &es) {
    ++phase2Examined;
    const auto &visited = es.getVisited();
    // Match write nodes by SOURCE POSITION, not pointer: a chain's nodes come from the SD dependency
    // graph's IR (and, with --state-dep-cache, from a re-resolved cache), whose pointers differ from
    // the executing program's — so visited.count(n) misses even for a node that plainly ran (the
    // .execute() call site included). isInRegisterActionBody keys on positions for the same reason.
    // Pointer identity is still tried first; positions are the fallback.
    std::unordered_set<cstring> visitedPositions;
    visitedPositions.reserve(visited.size() * 2);
    for (const auto *v : visited) visitedPositions.insert(cstring(v->getSourceInfo().toPositionString()));

    std::vector<size_t> matched;
    for (const auto *ch : allChains) {
        if (phase2Reached.count(ch->id)) continue;  // already known reachable
        auto it = chainPhase2Targets.find(ch->id);
        if (it == chainPhase2Targets.end() || it->second.empty()) continue;
        // Write-path coverage, SOUND TOWARD KEEPING: this pass may only prune chains whose write is
        // provably unreachable, so in-RegisterAction body nodes are excused UNCONDITIONALLY (unlike
        // chainTargetsCovered's soRan-gated excusal). A read-modify-write body's branches are mutually
        // exclusive — e.g. `exc = 1` under `if (v > 20)` cannot be covered by the same single packet
        // that also runs the v==0 path — so requiring them would prune every RMW chain, dropping real
        // tests. Only control-body nodes (the .execute() call site, outside any RegisterAction) stay
        // strictly required; an unreachable guard around the call site is exactly what we prune on.
        const bool covered =
            std::all_of(it->second.begin(), it->second.end(),
                        [&visited, &visitedPositions, &es, ch, this](const IR::Node *n) {
                            if (n->is<IR::Key>())
                                return isTableVisited(ch->sinkTableControlPlaneName, visited);
                            if (visited.count(n) > 0) return true;
                            if (visitedPositions.count(
                                    cstring(n->getSourceInfo().toPositionString())) > 0)
                                return true;
                            if (isInRegisterActionBody(n)) return true;
                            // Under the Cond policy cmd_stepper CLONES the IfStatement (it reduces
                            // the condition for branch stamping), so neither its pointer nor its
                            // position appears in `visited` even when the branch was taken — the sink
                            // condition, which the write chain ends at, would MISS on every path and
                            // prune every condition chain. It does stamp a source-position-keyed
                            // condition var when the branch is evaluated; treat that as coverage.
                            // exists() (not get()): get() BUGs on an unreached branch's missing var.
                            if (const auto *ifs = n->to<IR::IfStatement>())
                                return es.exists(CmdStepper::getConditionVar(ifs));
                            return false;
                        });
        if (covered) matched.push_back(ch->id);
    }
    if (matched.empty()) return;
    // One SAT check shared across every chain this terminal covers (the efficiency): an infeasible
    // path proves nothing, so it cannot rescue a chain from pruning.
    auto sat = solver.checkSat(es.getPathConstraint());
    if (!sat || !*sat) return;
    for (auto id : matched) phase2Reached.insert(id);
}

std::set<size_t> StateDependencyTracker::collectPhase2Terminals(const ExecutionState &initState) {
    phase2Reached.clear();
    chainPhase2Targets.clear();
    phase2Examined = 0;

    // Per-chain Phase-2 write targets (writeNodes) and their union.
    currentPhase = TamperingPhase::Phase2_Write;
    P4::Coverage::CoverageSet unionTargets;
    for (const auto *chain : allChains) {
        currentChain = chain;  // buildRequiredNodes consults currentPhase + chain
        auto targets = buildRequiredNodes(*chain);  // Phase2_Write -> chain.writeNodes
        for (const auto *n : targets) unionTargets.insert(n);
        chainPhase2Targets[chain->id] = std::move(targets);
    }
    currentChain = nullptr;
    if (unionTargets.empty()) return {};  // no write nodes to prove reachable -> prune nothing

    const size_t base = std::max<size_t>(static_cast<size_t>(SymbexOptions::get().maxTests), 1);
    phase2ExamineBudget = std::max<size_t>(allChains.size() * base * 4, 2000);

    // Steer toward the UNION of all chains' write nodes.
    currentRequiredNodes = unionTargets;
    // Hazard: reaching-set pruning cuts the path right after a target and can strand a chain whose
    // write is genuinely reachable only past that point — which would prune a real chain (unsound).
    // Disable it here as Phase-1/Phase-3 do for the condition policy; keep only the (non-pruning)
    // steering toward required nodes.
    reachingSet_.clear();
    reachingSetValid_ = false;

    solver.checkSat({});  // clear accumulated assertions before the write pass
    sharedPhase2 = true;
    phase2BudgetExhausted = false;
    seekMiss_ = false;
    currentSinkTable_ = nullptr;
    {
        // Unconstrained: allow drops + register zero-init + register tracking, NONE of the
        // per-(chain, terminal) machinery (no carry, index/port pinning, NEQ, size-1 configs). That
        // cheapness is the whole point — the pass only decides reachable-or-not.
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, {}, /*setRegTracking=*/true,
                               /*isPhase1=*/true);
        unexploredBranches.clear();
        auto &phase2Init = initState.clone();
        // No-op callback: reachability is recorded in runImpl/handleSharedPhase2Terminal.
        runImpl([](const FinalState &) -> bool { return false; }, phase2Init);
    }
    sharedPhase2 = false;
    currentChain = nullptr;

    // A chain may be pruned ONLY when the write-path search actually completed: an exhausted DFS
    // that never reached a chain's write proves it unreachable, whereas a budget-truncated one
    // proves nothing. Conflating the two would prune reachable chains on precisely the large
    // programs this pass targets — inflating "chains completed" while silently losing real tests.
    // Diagnostic: which chains' write paths the UNCONSTRAINED search actually covered. This is the
    // probe that separates "the write is hard to reach at all" from "the per-(chain, terminal)
    // constraints (index pinning / port NEQ / carry) are what block it" — this pass applies none.
    for (const auto *ch : allChains) {
        const bool hit = phase2Reached.count(ch->id) > 0;
        cstring wpos = ""_cs;
        auto it = chainPhase2Targets.find(ch->id);
        if (it != chainPhase2Targets.end())
            for (const auto *n : it->second)
                if (!isInRegisterActionBody(n))
                    wpos = cstring(n->getSourceInfo().toPositionString());
        printInfo("[Tampering] Phase-2 reach: chain id=%1% SO=%2% -> %3% (write %4%)", ch->id,
                  ch->soName, hit ? "REACHED" : "not reached", wpos);
    }

    if (phase2BudgetExhausted) {
        printInfo("[Tampering] Phase-2 prefilter: %1% chains, %2% reached, 0 pruned, %3% terminals "
                  "examined (budget reached before the write-path search completed; "
                  "unreached != unreachable)",
                  allChains.size(), phase2Reached.size(), phase2Examined);
        return {};
    }

    std::set<size_t> pruned;
    for (const auto *ch : allChains)
        if (phase2Reached.find(ch->id) == phase2Reached.end()) pruned.insert(ch->id);

    printInfo("[Tampering] Phase-2 prefilter: %1% chains, %2% pruned, %3% terminals examined "
              "(search completed)",
              allChains.size(), pruned.size(), phase2Examined);
    return pruned;
}

std::vector<const FinalState *> StateDependencyTracker::collectPhase1TerminalsPerChain(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState) {
    // Baseline for --shared-traversal=NONE. Deliberately mirrors collectPhase1Terminals in EVERY
    // respect except the traversal scope (this chain's targets, not the union of all chains') and the
    // bucketing scope (phase1SingleChain restricts handleSharedTerminal to this chain). Crucially it
    // reuses the SAME conditionReached / chainTargetsCovered bucketing predicate via runImpl — NOT
    // runPhase's allCovered acceptance, which rejects every RMW read terminal because the threshold
    // branch is unreachable in one packet — so the two modes produce identical terminals for this
    // chain. The HIT->MISS / MISS->HIT direction filters are applied downstream in runTamperingChain /
    // runConditionChain exactly as for the shared bucket, so one pass serves both.
    currentPhase = TamperingPhase::Phase1_Read;
    currentChain = &chain;
    auto targets = buildRequiredNodes(chain);
    if (chain.sinkConditionNode != nullptr) targets.insert(chain.sinkConditionNode);
    currentChain = nullptr;
    if (targets.empty()) return {};

    // Reset just this chain's bucket; same caps as the shared pass so bucket sizes are comparable.
    // phase1Examined is a member shared with the shared pass and is consumed by runImpl's budget
    // check — it MUST be zeroed per chain here, or chain 0 spends the whole budget and every later
    // chain returns on its first terminal with an empty bucket (silently starving the baseline).
    phase1Examined = 0;
    phase1Buckets[chain.id].clear();
    chainPhase1Targets[chain.id] = targets;  // chainTargetsCovered (key chains) consults this
    const size_t base = std::max<size_t>(static_cast<size_t>(SymbexOptions::get().maxTests), 1);
    phase1BucketCap = std::max<size_t>(base * 4, 12);
    phase1ExamineBudget = std::max<size_t>(phase1BucketCap * allChains.size() * 4, 2000);

    // Steer toward THIS chain's targets only (the scope difference from the shared union pass).
    currentRequiredNodes = targets;
    // Reaching-set pruning: same opt-out as the shared pass (it cuts the path right after a target,
    // which strands condition chains that must continue past the if-statement to a terminal).
    if (policy == StateDependencyPolicy::TamperingCond) {
        reachingSet_.clear();
        reachingSetValid_ = false;
    } else {
        buildReachingSet();
    }

    solver.checkSat({});  // clear accumulated assertions before this chain's read pass
    sharedPhase1 = true;
    phase1SingleChain = &chain;  // restrict handleSharedTerminal's bucketing to this chain
    seekMiss_ = false;
    currentSinkTable_ = nullptr;
    {
        // Identical guard to the shared pass: allow drops (serves both directions) + register
        // zero-init + register tracking.
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, {}, /*setRegTracking=*/true,
                               /*isPhase1=*/true);
        unexploredBranches.clear();
        auto &phase1Init = initState.clone();
        // No-op callback: bucketing happens in runImpl/handleSharedTerminal, not the callback.
        runImpl([](const FinalState &) -> bool { return false; }, phase1Init);
    }
    sharedPhase1 = false;
    phase1SingleChain = nullptr;
    currentChain = nullptr;

    auto out = phase1Buckets[chain.id];
    printInfo("[Tampering] Per-chain Phase-1: chain id=%1%, %2% bucketed (cap %3%)", chain.id,
              out.size(), phase1BucketCap);
    return out;
}

// ---------------------------------------------------------------------------
// Sink-state / disposition helpers
// ---------------------------------------------------------------------------

int StateDependencyTracker::evalSinkHit(const FinalState *fs) const {
    if (currentSinkTable_ == nullptr) return -1;
    const auto &hitVar = TableStepper::getTableHitVar(currentSinkTable_);
    const auto *hitExpr = fs->getExecutionState()->get(hitVar);
    // -1: the sink table was never applied on this path (e.g. the packet dropped earlier). Such a
    // terminal cannot flip MISS→HIT under tampering, so the Phase-1 filter discards it (keeps only
    // reached-and-MISS terminals). 0 = reached and MISSed, 1 = reached and HIT.
    if (hitExpr == nullptr) return -1;
    const auto *hitVal = fs->getFinalModel().evaluate(hitExpr, true);
    const auto *hitBool = hitVal->to<IR::BoolLiteral>();
    if (hitBool == nullptr) return -1;
    return hitBool->value ? 1 : 0;
}

int StateDependencyTracker::evalCondition(const FinalState *fs) const {
    if (currentSinkCondition == nullptr) return -1;
    // CmdStepper stamps this boolean (under the TamperingCond policy) to true/false for the
    // then/else branch; it is unset if the if-statement was not reached on this path.
    const auto &condVar = CmdStepper::getConditionVar(currentSinkCondition);
    const auto *es = fs->getExecutionState();
    // exists() BEFORE get(): an if-statement that was not reached on this path leaves the condition
    // var unstamped, and get() does not return nullptr for a missing var — it BUGs ("Unable to find
    // var ... in the symbolic environment") and aborts the entire run, discarding every test already
    // found. Same hazard the isConditionReached()/nodeCovered() helpers above already guard against.
    if (!es->exists(condVar)) return -1;  // condition not reached
    const auto *condExpr = es->get(condVar);
    if (condExpr == nullptr) return -1;  // condition not reached
    // A tainted condition (e.g. gated on a read from a RANDOM-hash-indexed sketch cell) is NOT a
    // determined flip: evaluate(doComplete=true) would fabricate an arbitrary truth value, so the
    // single-packet write DFS "confirms" a flip the concrete re-derivation can't reproduce (the
    // emitted counter only reaches a small value, never the threshold). Report unresolved so the
    // caller routes to the analytical accumulation path, which pre-sets a concrete SO value.
    if (Taint::hasTaint(condExpr)) return -1;
    const auto *condVal = fs->getFinalModel().evaluate(condExpr, true);
    const auto *condBool = condVal->to<IR::BoolLiteral>();
    if (condBool == nullptr) return -1;
    return condBool->value ? 1 : 0;  // 1 = then (true), 0 = else (false)
}

int StateDependencyTracker::evalSinkFlip(const FinalState *fs) const {
    if (currentSinkTable_ != nullptr) return evalSinkHit(fs);
    if (currentSinkCondition != nullptr) return evalCondition(fs);
    return -1;
}

void StateDependencyTracker::evalDisposition(const FinalState *fs, bool &dropped,
                                             int &outPort) const {
    const auto *es = fs->getExecutionState();
    // Mirrors runPhase's drop predicate: no output bytes or the drop property ⇒ dropped.
    dropped = es->getPacketBufferSize() <= 0 || es->getProperty<bool>("drop"_cs);
    outPort = -1;
    if (dropped) return;
    const auto *opExpr = es->get(programInfo.getTargetOutputPortVar());
    if (opExpr == nullptr || Taint::hasTaint(opExpr)) {
        // A tainted/unset egress port means the packet is dropped (see check_tofino_drop).
        dropped = true;
        return;
    }
    outPort = IR::getIntFromLiteral(fs->getFinalModel().evaluate(opExpr, true));
}

int StateDependencyTracker::evalMulticastGroup(const FinalState *fs) const {
    const auto *es = fs->getExecutionState();
    // Return the first non-zero, untainted multicast group id among the target's mcast vars.
    for (const auto *mcastVar : programInfo.getMulticastGroupVars()) {
        const auto *mcastExpr = es->get(*mcastVar);
        if (mcastExpr == nullptr || Taint::hasTaint(mcastExpr)) {
            continue;
        }
        int mgid = IR::getIntFromLiteral(fs->getFinalModel().evaluate(mcastExpr, true));
        if (mgid != 0) {
            return mgid;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Per-chain three-phase scenario (both tamper directions; Phase 1/2 shared)
// ---------------------------------------------------------------------------

std::pair<int, int> StateDependencyTracker::getPortPair(const FinalState *fs) const {
    const auto &model = fs->getFinalModel();
    const auto *es = fs->getExecutionState();
    int ip = IR::getIntFromLiteral(model.evaluate(es->get(programInfo.getTargetInputPortVar()), true));
    int op = IR::getIntFromLiteral(model.evaluate(es->get(programInfo.getTargetOutputPortVar()), true));
    return {ip, op};
}

namespace {
/// Collects the packet-field SymbolicVariable leaves of a register-index expression. An
/// IR::ConcolicVariable (e.g. a Concolic_Hash_get of a CRC) IS-A SymbolicVariable, but the hash
/// OPERANDS we want live in its `arguments` — so recurse into those and do NOT collect the concolic
/// node itself. Plain SymbolicVariables (pktvar_N) are the leaves we want.
class IndexSymVarCollector : public Inspector {
 public:
    std::set<const IR::SymbolicVariable *> vars;
    bool preorder(const IR::ConcolicVariable *cv) override {
        if (cv->arguments != nullptr) visit(cv->arguments);
        return false;  // skip the concolic node itself; we collected its operands above
    }
    bool preorder(const IR::SymbolicVariable *sv) override {
        vars.insert(sv);
        return false;
    }
};

// The three pins below are `this`-free, so their bodies live here as file-local statics and the
// member functions are one-line forwarders. That lets applyPhase2IndexPolicy — which has no `this`,
// because keeping it out of the header avoids a whole-tree rebuild through lib/test_backend.h —
// share exactly the same code as the members' external callers.

std::set<const IR::SymbolicVariable *> collectIndexSymVarsImpl(const TestObject *soReg) {
    IndexSymVarCollector collector;
    if (soReg != nullptr) {
        for (const auto *idx : soReg->getIndexExpressions()) {
            if (idx != nullptr) idx->apply(collector);
        }
    }
    return collector.vars;
}

void pinIndexInputsToPhase1Impl(ExecutionState &init, const FinalState *fs1,
                                const std::set<const IR::SymbolicVariable *> &symVars) {
    const auto &model1 = fs1->getFinalModel();
    for (const auto *sv : symVars) {
        init.pushPathConstraint(new IR::Equ(sv, model1.evaluate(sv, true)));
    }
}

void pinPacketToPhase1Impl(ExecutionState &init, const FinalState *fs1, int inputPort,
                           const IR::Expression *inputPortSymExpr) {
    const auto &model1 = fs1->getFinalModel();
    const auto *p1PktExpr = fs1->getExecutionState()->getInputPacket();
    const auto *p1PktSize = model1.evaluate(ExecutionState::getInputPacketSizeVar(), true);
    // The accumulated input-packet expression is a Concat tree rooted at a zero-width constant, which
    // Z3 cannot translate; so pin each pktvar_N *symbolic variable* it contains to its @p fs1 model
    // value. Cloning initState re-pulls the same pktvar_N in order, so this replays fs1's exact bytes
    // (same header-derived hash inputs + key fields), plus the packet size and input port.
    std::function<void(const IR::Expression *)> pinPktVars = [&](const IR::Expression *e) {
        if (e == nullptr) return;
        if (const auto *sv = e->to<IR::SymbolicVariable>()) {
            init.pushPathConstraint(new IR::Equ(sv, model1.evaluate(sv, true)));
        } else if (const auto *cc = e->to<IR::Concat>()) {
            pinPktVars(cc->left);
            pinPktVars(cc->right);
        } else if (const auto *sl = e->to<IR::Slice>()) {
            pinPktVars(sl->e0);
        }
    };
    pinPktVars(p1PktExpr);
    init.pushPathConstraint(new IR::Equ(ExecutionState::getInputPacketSizeVar(), p1PktSize));
    init.pushPathConstraint(
        new IR::Equ(inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, inputPort)));
}

/// Classifies the Phase-1 index of state object @p soName and constrains @p phase2Init so Phase 2
/// writes the cell Phase 1 read:
///   - packet-derived index (e.g. a concolic CRC hash): pin ONLY the index-determining inputs to
///     Phase 1 so the attacker hits the victim's bucket; keep the port NEQ and DROP the table-key
///     NEQ (it would conflict with a pinned hash-operand key). Other fields stay free.
///   - tainted index (RANDOM hash, operands unrecoverable): pin the whole packet (same flow/path).
///   - constant index: full distinctness (port + table-key NEQ), i.e. two coexisting entries.
/// The port NEQ makes Phase 2 enter on a port that is neither Phase 1's ingress nor its egress; a
/// dropped Phase-1 baseline has no output port (@p cond1 .outputPort < 0), so only the input NEQ
/// applies. Table-key NEQs skip @p size1Tables, whose single entry is pre-injected from Phase 1.
///
/// The key (H2S2K) and condition (H2S2C) chains ran byte-identical copies of this; an index policy
/// that drifts between the two is a silent unsoundness, so both go through this one helper.
///
/// @p skipIndexPin keeps the port NEQ but drops the index-input pin, for a caller that constrains
/// the index itself and must not inherit Phase 1's hash-operand pktvar equalities as an extra
/// constraint that can make its query UNSAT.
/// Sets @p indexIsPacketDerived when the index resolved to packet-field leaves (the first case
/// above) and returns Phase 1's register test object, so the caller can inspect the very index
/// expressions the policy classified.
const TestObject *applyPhase2IndexPolicy(
    ExecutionState &phase2Init, const FinalState *repPhase1State, const PhaseConditions &cond1,
    const IR::Expression *inputPortSymExpr, const std::vector<cstring> &size1Tables, cstring soName,
    [[maybe_unused]] const std::unordered_map<cstring, const IR::P4Table *> &tableByName,
    bool skipIndexPin, /*out*/ bool &indexIsPacketDerived) {
    const TestObject *soReg =
        (repPhase1State != nullptr)
            ? repPhase1State->getExecutionState()->getTestObject("registervalues"_cs, soName,
                                                                 /*checked=*/false)
            : nullptr;
    const auto indexSymVars = collectIndexSymVarsImpl(soReg);
    indexIsPacketDerived = !indexSymVars.empty();
    if (indexSymVars.empty() && soReg != nullptr && soReg->hasTaintedIndex()) {
        pinPacketToPhase1Impl(phase2Init, repPhase1State, cond1.inputPort, inputPortSymExpr);
        return soReg;
    }
    phase2Init.pushPathConstraint(
        new IR::Neq(inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
    if (cond1.outputPort >= 0)
        phase2Init.pushPathConstraint(new IR::Neq(
            inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
    if (indexIsPacketDerived) {
        if (!skipIndexPin) pinIndexInputsToPhase1Impl(phase2Init, repPhase1State, indexSymVars);
    } else {
        const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
        for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
            if (size1Set.count(tblName) > 0) continue;
            for (const auto &[keyName, match] : keyMap)
                phase2Init.pushPathConstraint(match->buildTableKeyNeqConstraint(tblName, keyName));
        }
    }
    return soReg;
}

// ---------------------------------------------------------------------------
// Phase-2 write-cell soundness: concrete cells, hash compatibility, steer-else-drop
//
// A tampering case is realizable only if the attacker's Phase-2 packet writes the very register
// cell the victim's Phase-1 packet read. pinIndexInputsToPhase1 pins the index-determining
// SymbolicVariable LEAVES, which is variable-wise rather than value-wise: it means "same cell" only
// when both phases index through the same hash over the same fields. Phases that hash DIFFERENT
// fields — or the same field bound to different pktvars because Phase 2 took another parse path —
// satisfy that pin and still land in different buckets. So the cells are compared CONCRETELY here,
// and a mismatch is first steered (re-derive Phase 2 with the victim's hash operands), then dropped
// if steering is impossible, out of budget, or still lands elsewhere.
// ---------------------------------------------------------------------------

/// Every IR::ConcolicVariable directly reachable in an expression. ConcolicVariable::visit_children
/// only visits the type, so hash arguments are not descended into — the same scope the concolic
/// resolver itself operates at, which is what makes the collected nodes the actual hash call sites.
class ConcolicVarCollector : public Inspector {
 public:
    std::vector<const IR::ConcolicVariable *> vars;
    bool preorder(const IR::ConcolicVariable *cv) override {
        vars.push_back(cv);
        return false;
    }
};

std::vector<const IR::ConcolicVariable *> collectConcolicVars(const IR::Expression *expr) {
    ConcolicVarCollector collector;
    if (expr != nullptr) expr->apply(collector);
    return collector.vars;
}

/// Argument layout of a concolic hash call, keyed by concolic method name so core stays
/// target-agnostic (it never mentions Tofino's Hash or v1model's hash extern, only their concolic
/// names). @c dataSlots hold the hashed input — the operands steering re-derives — and
/// @c paramSlots the structural parameters two call sites must agree on to compute the same
/// function. Unlisted positions are deliberately ignored: `*method_hash`'s slot 0 is the output
/// lvalue, which names the destination field and says nothing about the function computed.
struct HashArgLayout {
    std::vector<size_t> dataSlots;
    std::vector<size_t> paramSlots;
};

const std::map<cstring, HashArgLayout> &hashArgLayouts() {
    static const std::map<cstring, HashArgLayout> LAYOUTS{
        // Tofino/TNA: Hash<W>(algo[, poly]).get(data) — targets/tofino/concolic.cpp.
        {"Hash_get"_cs, HashArgLayout{{0}, {}}},
        // bmv2/v1model: hash(result, algo, base, data, max) — targets/bmv2/concolic.cpp.
        {"*method_hash"_cs, HashArgLayout{{3}, {1, 2, 4}}},
    };
    return LAYOUTS;
}

/// The individual operands a hash's data argument is built from. Both targets accept either a
/// struct expression (the usual `{f1, f2}` field list) or a single scalar.
std::vector<const IR::Expression *> flattenHashData(const IR::Expression *dataExpr) {
    if (dataExpr == nullptr) return {};
    if (const auto *structExpr = dataExpr->to<IR::StructExpression>()) {
        return IR::flattenStructExpression(structExpr);
    }
    return {dataExpr};
}

/// A COPY of @p fs's final model with a binding added for every concolic variable that @p exprs
/// resolve to under it, @p resolvedAny reporting whether any binding was actually added.
///
/// Binding them is what makes an index expression evaluable at all. A hash index is an
/// IR::ConcolicVariable, and unless something on the path had to read the hash the solver never
/// assigned its label — so `evaluate(idx, /*doComplete=*/true)` substitutes the type's default and
/// every hash-indexed register reads cell 0. Resolution goes through the registered concolic
/// implementations, which both targets have, so a CRC index folds on bmv2 (`*method_hash`) and on
/// Tofino (`Hash_get`) without core knowing either extern; Tofino's eager folding
/// (tryComputeConcreteHash) is target-private and unreachable from here.
///
/// The bindings are computed FROM this model, so they resolve the index rather than choose it: the
/// cell is the hash of the packet the model has already fixed. Throws whatever the concolic
/// implementation throws on an unimplemented flavour or an untranslatable operand.
Model &modelWithResolvedConcolics(const FinalState *fs,
                                  const std::vector<const IR::Expression *> &exprs,
                                  const ProgramInfo &programInfo, bool &resolvedAny) {
    const auto &model = fs->getFinalModel();
    auto *resolved = new Model(model);
    resolvedAny = false;
    ConcolicResolver resolver(model, *fs->getExecutionState(),
                              *programInfo.getConcolicMethodImpls());
    for (const auto *expr : exprs) {
        if (expr == nullptr || Taint::hasTaint(expr)) continue;
        expr->apply(resolver);
    }
    for (const auto &[var, value] : *resolver.getResolvedConcolicVariables()) {
        // Only a concolic variable carries a label the model can be keyed by; a whole-expression
        // key has no symbol to bind.
        if (!std::holds_alternative<IR::ConcolicVariable>(var)) continue;
        resolved->set(std::get<IR::ConcolicVariable>(var).clone(), value);
        resolvedAny = true;
    }
    return *resolved;
}

/// The concrete register cell @p idx addresses in @p fs, RAW (unmasked).
/// nullopt on taint, on an unresolvable method, or on anything that does not fold to a constant.
///
/// RAW is deliberate. maskIndex (the register's real address space) is applied only when the test
/// is emitted, and raw equality is exactly what the steering constraint produces — accepting on the
/// same criterion we enforce is the only self-consistent choice. Raw equality implies masked
/// equality, so the gate is a sound under-approximation of "same cell".
std::optional<big_int> evalConcreteIndex(const FinalState *fs, const IR::Expression *idx,
                                         const ProgramInfo &programInfo) {
    if (fs == nullptr || idx == nullptr) return std::nullopt;
    if (Taint::hasTaint(idx)) return std::nullopt;
    try {
        bool resolvedAny = false;
        const auto &idxModel = modelWithResolvedConcolics(fs, {idx}, programInfo, resolvedAny);
        if (const auto *folded = idxModel.evaluate(idx, /*doComplete=*/true)->to<IR::Constant>()) {
            return folded->value;
        }
    } catch (const std::exception &) {
        // An unimplemented hash flavour, an untranslatable operand, or an expression that does not
        // fold: unknown, which is not the same as unequal — the caller treats it as a match.
    }
    return std::nullopt;
}

/// The register test object @p fs holds for state object @p soName, or nullptr.
const TestObject *soRegisterOf(const FinalState *fs, cstring soName) {
    if (fs == nullptr) return nullptr;
    return fs->getExecutionState()->getTestObject("registervalues"_cs, soName, /*checked=*/false);
}

/// @p soReg's access-index expressions, nullptr slots removed; with @p writesOnly, only the ones it
/// was WRITTEN at.
///
/// getIndexExpressions() has a positional contract: slot 0 is the read/initial index — a nullptr
/// placeholder when the register has none — and every later slot is a recorded write index. So the
/// writes are exactly "everything past slot 0", unconditionally. Do NOT make the head-drop depend on
/// how many slots came back: a v1model `register.write(i1, ..); register.write(i2, ..)` with no
/// preceding read returns {nullptr, i1, i2}, and a size-based rule would drop i1 and let a write to
/// the wrong cell through the gate. (Tofino never showed this: a RegisterAction always reads before
/// it writes, so its slot 0 is always a real index.)
///
/// The read/write distinction matters for the split-access shape (`register.read(v, i1)` then
/// `register.write(i2, v)`), where accepting on the read index would let a write elsewhere pass; a
/// RegisterAction reads and writes one index, so there the two views coincide.
std::vector<const IR::Expression *> indexExpressionsOf(const TestObject *soReg, bool writesOnly) {
    if (soReg == nullptr) return {};
    auto idxExprs = soReg->getIndexExpressions();
    if (writesOnly && !idxExprs.empty()) idxExprs.erase(idxExprs.begin());
    idxExprs.erase(std::remove(idxExprs.begin(), idxExprs.end(), nullptr), idxExprs.end());
    return idxExprs;
}

/// Every concrete cell @p fs WRITES for @p soName. nullopt means "unknown" — no index at all, or
/// one that does not resolve — which callers treat as a match: an unresolvable (tainted) index
/// already took the whole-packet pin, which guarantees the same cell, and dropping those would zero
/// every RANDOM-hash-indexed program.
std::optional<std::set<big_int>> concreteWriteCells(const FinalState *fs, cstring soName,
                                                    const ProgramInfo &programInfo) {
    const auto idxExprs = indexExpressionsOf(soRegisterOf(fs, soName), /*writesOnly=*/true);
    if (idxExprs.empty()) return std::nullopt;
    std::set<big_int> cells;
    for (const auto *idx : idxExprs) {
        auto cell = evalConcreteIndex(fs, idx, programInfo);
        if (!cell.has_value()) return std::nullopt;
        cells.insert(*cell);
    }
    return cells;
}

/// Phase 1's READ index expression: slot 0 of getIndexExpressions() is the initial/read index, which
/// for the victim is the cell whose value reaches the sink. A register written without ever being
/// read has no slot-0 index; there the first write index is the best available answer, and for the
/// read-modify-write shape it is the same expression anyway.
const IR::Expression *readIndexExpression(const FinalState *fs, cstring soName) {
    const auto idxExprs = indexExpressionsOf(soRegisterOf(fs, soName), /*writesOnly=*/false);
    return idxExprs.empty() ? nullptr : idxExprs.front();
}

/// The model to fold state object @p soName's register through when the attacker test object is
/// built: @p fs's final model plus a binding for every concolic variable its index expressions
/// resolve to.
///
/// THIS is what puts a real cell in the emitted `affected_register.index`. That field is
/// concretised by TestObject::withAttackerValues(), which evaluates the index against whatever
/// model it is handed — and that call happens while the tampering final state is being built, long
/// before the test backend re-solves the phase. Resolving the index anywhere downstream of here
/// cannot reach it.
///
/// Bindings are set(), not mergeMap()'d: when the path did branch on the hash, the solver's own
/// assignment is whatever satisfied that branch, while the concolic implementation gives the cell
/// the packet genuinely addresses — and the cell the hardware will touch is the one the harness
/// needs. Such a case is exactly the one the emission side reports as not enforced.
const Model &modelWithResolvedIndices(const FinalState *fs, cstring soName,
                                      const ProgramInfo &programInfo) {
    const auto idxExprs = indexExpressionsOf(soRegisterOf(fs, soName), /*writesOnly=*/false);
    if (idxExprs.empty()) return fs->getFinalModel();
    try {
        bool resolvedAny = false;
        const auto &resolved = modelWithResolvedConcolics(fs, idxExprs, programInfo, resolvedAny);
        if (resolvedAny) return resolved;
    } catch (const std::exception &) {
        // An unimplemented hash flavour or an untranslatable operand: fall through and leave the
        // model alone. The emitted index is then whatever the solver reported, exactly as before.
    }
    return fs->getFinalModel();
}

/// Two hash call sites are interchangeable when equal inputs imply equal outputs, so steering one
/// onto the other's operand values really lands in the same cell. Structural, on the concolic
/// nodes: same method, same result type, same argument count, every structural parameter
/// equivalent, the hashed input of the same shape, and every associated declaration (the extern
/// instance, plus a CRCPolynomial for a custom algorithm) built from equivalent constructor
/// arguments.
///
/// srcIdentifier is deliberately NOT compared: two instances with identical parameters *are* the
/// same function, which is the entire point — it is what makes SwitchV2P's two
/// `Hash<index_t>(CRC32)` instances interchangeable while a differing polynomial stays unequal.
bool hashesAreInterchangeable(const IR::ConcolicVariable *a, const IR::ConcolicVariable *b) {
    if (a == nullptr || b == nullptr) return false;
    if (a->concolicMethodName != b->concolicMethodName) return false;
    const auto &layouts = hashArgLayouts();
    auto layoutIt = layouts.find(a->concolicMethodName);
    if (layoutIt == layouts.end()) return false;  // unknown method ⇒ not steerable ⇒ drop
    if (a->type == nullptr || b->type == nullptr || !a->type->equiv(*b->type)) return false;
    if (a->arguments == nullptr || b->arguments == nullptr) return false;
    if (a->arguments->size() != b->arguments->size()) return false;
    for (auto slot : layoutIt->second.paramSlots) {
        if (slot >= a->arguments->size()) return false;
        const auto *pa = a->arguments->at(slot)->expression;
        const auto *pb = b->arguments->at(slot)->expression;
        if (pa == nullptr || pb == nullptr || !pa->equiv(*pb)) return false;
    }
    // The hashed input must have the same SHAPE (component count and types). Its values are what
    // steering equates, so comparing them here would reject exactly the cases worth steering.
    for (auto slot : layoutIt->second.dataSlots) {
        if (slot >= a->arguments->size()) return false;
        const auto dataA = flattenHashData(a->arguments->at(slot)->expression);
        const auto dataB = flattenHashData(b->arguments->at(slot)->expression);
        if (dataA.empty() || dataA.size() != dataB.size()) return false;
        for (size_t i = 0; i < dataA.size(); ++i) {
            // Equivalent types, not merely equal widths: steering emits an Equ over these two
            // operands, and the solver backend needs both sides to be the same sort.
            if (dataA[i]->type == nullptr || dataB[i]->type == nullptr) return false;
            if (!dataA[i]->type->equiv(*dataB[i]->type)) return false;
        }
    }
    if (a->associatedNodes.size() != b->associatedNodes.size()) return false;
    for (size_t i = 0; i < a->associatedNodes.size(); ++i) {
        const auto *declA = a->associatedNodes.at(i)->to<IR::Declaration_Instance>();
        const auto *declB = b->associatedNodes.at(i)->to<IR::Declaration_Instance>();
        if (declA == nullptr || declB == nullptr) return false;
        if (declA->arguments == nullptr || declB->arguments == nullptr) return false;
        if (declA->arguments->size() != declB->arguments->size()) return false;
        for (size_t k = 0; k < declA->arguments->size(); ++k) {
            const auto *argA = declA->arguments->at(k)->expression;
            const auto *argB = declB->arguments->at(k)->expression;
            if (argA == nullptr || argB == nullptr || !argA->equiv(*argB)) return false;
        }
    }
    return true;
}

/// Equalities that force @p target's hash operands to the values @p sourceModel assigns
/// @p source's, i.e. "the attacker's key field takes the victim's key value". Empty when any
/// target operand is tainted (the Z3 backend cannot translate an equality over taint), the layout
/// is unknown, or a source operand does not evaluate — all of which mean "not steerable", which
/// the caller turns into a drop rather than a wrong-cell acceptance.
std::vector<const IR::Expression *> buildHashInputPins(const IR::ConcolicVariable *target,
                                                       const IR::ConcolicVariable *source,
                                                       const Model &sourceModel) {
    auto layoutIt = hashArgLayouts().find(target->concolicMethodName);
    if (layoutIt == hashArgLayouts().end()) return {};
    std::vector<const IR::Expression *> pins;
    try {
        for (auto slot : layoutIt->second.dataSlots) {
            const auto dataTarget = flattenHashData(target->arguments->at(slot)->expression);
            const auto dataSource = flattenHashData(source->arguments->at(slot)->expression);
            if (dataTarget.size() != dataSource.size()) return {};
            for (size_t i = 0; i < dataTarget.size(); ++i) {
                if (Taint::hasTaint(dataTarget[i]) || Taint::hasTaint(dataSource[i])) return {};
                pins.push_back(
                    new IR::Equ(dataTarget[i], sourceModel.evaluate(dataSource[i], true)));
            }
        }
    } catch (const std::exception &) {
        // A victim operand that does not fold to a literal (an unmodelled nested expression):
        // there is no concrete value to steer the attacker onto.
        return {};
    }
    return pins;
}

/// Drops every Phase-2 terminal in @p phase2Terminals whose write lands in a register cell Phase 1
/// never read, after trying to STEER it there: the attacker's hash operands are pinned to the
/// victim's values and the Phase-2 DFS is re-run from @p steerTemplate (a pristine clone — runPhase
/// mutates its root — built with skipIndexPin so it does not inherit Phase 1's operand pin, which
/// for a phase hashing another field is an unrelated extra constraint that can make the retry
/// UNSAT). Budgeted by --max-index-steer-retries, one retry per distinct hash call site, since a
/// program writing the register from several sites needs one steer per site.
///
/// A retry terminal is never trusted on the pushed equality alone: a retry that takes a different
/// parse path binds different pktvars to the same header field, making the equality vacuous. Every
/// retry terminal therefore goes back through the same concrete-cell check.
///
/// Terminals in @p drivenTerminals are accept-or-drop with no retry: a k-packet accumulation into
/// the wrong cell is unambiguously unsound, and re-running the DFS would discard the analytical k.
void gatePhase2ByIndex(const P4StateDependency::DependencyGraphs::SOChain &chain, cstring chainName,
                       const ProgramInfo &programInfo, const FinalState *repPhase1State,
                       std::vector<const FinalState *> &phase2Terminals,
                       ExecutionState &steerTemplate,
                       const std::function<void(ExecutionState &,
                                                std::vector<const FinalState *> &, size_t)>
                           &runPhase2,
                       size_t maxPerChain,
                       const std::map<const FinalState *, const FinalState *> &drivenTerminals) {
    if (phase2Terminals.empty()) return;
    // What Phase 3's replay reads back is the cell Phase 1 READ, so that is the cell the attacker
    // has to write. For a RegisterAction the read and the write index are one expression, so the
    // distinction only bites on the split `read(v, i)` / `write(j, v)` shape — where writing j
    // leaves the value the replay reads at i untouched, i.e. no tampering at all.
    const auto *p1Index = readIndexExpression(repPhase1State, chain.soName);
    const auto p1ReadCell = evalConcreteIndex(repPhase1State, p1Index, programInfo);
    // Unknown Phase-1 cell (no index, taint, or an unresolvable hash flavour): accept the bucket.
    // A tainted index already took the whole-packet pin, which guarantees the same cell, and
    // dropping those would zero every RANDOM-hash-indexed program.
    if (!p1ReadCell.has_value()) return;
    const std::set<big_int> p1Cells{*p1ReadCell};
    const auto p1Hashes = collectConcolicVars(p1Index);

    /// True when @p fs2 writes one of Phase 1's cells, or when there is nothing to gate.
    auto writesPhase1Cell = [&](const FinalState *fs2) {
        const auto *soReg = soRegisterOf(fs2, chain.soName);
        // No write of its own: the object is Phase 1's carried snapshot, and the emission loop
        // drops such a terminal anyway ("recorded no write"). Gating it only burns steer budget.
        if (soReg == nullptr || !soReg->wasWritten()) return true;
        const auto p2Cells = concreteWriteCells(fs2, chain.soName, programInfo);
        if (!p2Cells.has_value()) return true;
        return std::any_of(p2Cells->begin(), p2Cells->end(),
                           [&](const big_int &c) { return p1Cells.count(c) > 0; });
    };

    /// The first written index expression of @p fs2 that addresses none of Phase 1's cells — the
    /// write steering has to move.
    auto mismatchedIndex = [&](const FinalState *fs2) -> const IR::Expression * {
        for (const auto *idx :
             indexExpressionsOf(soRegisterOf(fs2, chain.soName), /*writesOnly=*/true)) {
            auto cell = evalConcreteIndex(fs2, idx, programInfo);
            if (cell.has_value() && p1Cells.count(*cell) == 0) return idx;
        }
        return nullptr;
    };

    int64_t budget = SymbexOptions::get().maxIndexSteerRetries;
    std::set<cstring> steeredSites;
    // Operand pins accumulate across retries: steering a second call site must not undo the first,
    // and the pins cannot contradict each other because every one of them names a victim value.
    std::vector<const IR::Expression *> steerPins;
    std::vector<const FinalState *> accepted;
    std::vector<const FinalState *> pending = phase2Terminals;
    size_t dropped = 0;
    while (!pending.empty()) {
        std::vector<const FinalState *> mismatched;
        for (const auto *fs2 : pending) {
            if (writesPhase1Cell(fs2)) {
                accepted.push_back(fs2);
            } else {
                mismatched.push_back(fs2);
            }
        }
        pending.clear();
        dropped += mismatched.size();
        bool retried = false;
        for (const auto *fs2 : mismatched) {
            if (budget <= 0 || drivenTerminals.count(fs2) > 0) continue;
            const auto *p2Index = mismatchedIndex(fs2);
            if (p2Index == nullptr) continue;
            const auto p2Hashes = collectConcolicVars(p2Index);
            // Two shapes are steerable. A concolic hash on both sides: pin the attacker's operands
            // to the victim's operand values, which is what actually moves the packet. A plain
            // arithmetic index on both sides (no concolic node anywhere): pin the index expression
            // itself to the victim's cell. Anything else — notably an opaque hash label on one side
            // only — cannot be steered: pinning a concolic label constrains no packet field.
            std::vector<const IR::Expression *> pins;
            const IR::Expression *valuePin = nullptr;
            cstring site;
            if (!p2Hashes.empty() && !p1Hashes.empty()) {
                if (!hashesAreInterchangeable(p2Hashes.front(), p1Hashes.front())) continue;
                pins = buildHashInputPins(p2Hashes.front(), p1Hashes.front(),
                                          repPhase1State->getFinalModel());
                if (pins.empty()) continue;
                site = p2Hashes.front()->label;
            } else if (p2Hashes.empty() && p1Hashes.empty()) {
                site = cstring(p2Index->toString());
            } else {
                continue;
            }
            if (steeredSites.count(site) > 0) continue;
            // Value pin (Step 3's pattern one level down): when the phases share a hash call site,
            // asserting the index itself steers the DFS straight at the victim's cell. It is only
            // an accelerator — the concrete-cell check below is what accepts or drops — so an
            // attempt that yields no terminal simply falls back to the operand pins alone.
            // Bit types only: the cell is a bit pattern, and width_bits() is a BUG on the
            // width-less types (Type_InfInt) an unlowered index could still carry.
            const auto *p2IndexType =
                p2Index->type != nullptr ? p2Index->type->to<IR::Type_Bits>() : nullptr;
            const auto *p1IndexType =
                p1Index->type != nullptr ? p1Index->type->to<IR::Type_Bits>() : nullptr;
            if (p2IndexType != nullptr && p1IndexType != nullptr &&
                p2IndexType->width_bits() == p1IndexType->width_bits()) {
                valuePin = new IR::Equ(p2Index, IR::Constant::get(p2IndexType, *p1ReadCell));
            }
            if (pins.empty() && valuePin == nullptr) continue;  // nothing to steer with
            steeredSites.insert(site);
            --budget;
            std::vector<const IR::Expression *> attemptPins = steerPins;
            attemptPins.insert(attemptPins.end(), pins.begin(), pins.end());
            // Enforce the value pin if the path admits it, else fall back to the operand pins
            // alone, so a value pin the path cannot satisfy costs a retry rather than the case.
            std::vector<std::vector<const IR::Expression *>> attempts;
            if (valuePin != nullptr) {
                auto withValuePin = attemptPins;
                withValuePin.push_back(valuePin);
                attempts.push_back(withValuePin);
            }
            if (!pins.empty()) attempts.push_back(attemptPins);
            std::vector<const FinalState *> retryTerminals;
            for (const auto &constraints : attempts) {
                if (!retryTerminals.empty()) break;
                auto &retryInit = steerTemplate.clone();
                for (const auto *pin : constraints) retryInit.pushPathConstraint(pin);
                runPhase2(retryInit, retryTerminals, maxPerChain);
            }
            printInfo("[Tampering] chain id=%1% (%2%): Phase-2 wrote a cell Phase 1 never read; "
                      "steered hash call site '%3%' to the victim's operands — %4% retry "
                      "terminal(s), %5% retry budget left.",
                      chain.id, chainName, site, retryTerminals.size(), budget);
            steerPins = attemptPins;
            pending.insert(pending.end(), retryTerminals.begin(), retryTerminals.end());
            retried = true;
            // Classify what this retry produced before spending more budget: its terminals may
            // already be on the victim's cell, or may expose the next call site to steer.
            break;
        }
        if (!retried) break;
    }
    if (dropped > 0) {
        printInfo("[Tampering] chain id=%1% (%2%): dropped %3% Phase-2 terminal(s) writing a cell "
                  "of '%4%' that Phase 1 never read (unrealizable tampering); %5% kept.",
                  chain.id, chainName, dropped, chain.soName, accepted.size());
    }
    phase2Terminals = accepted;
}

/// Re-check, on the pair actually being emitted, that Phase 2 writes the cell Phase 1 read.
/// reDeriveConcretePhase substitutes terminals the Phase-2 gate never saw, and the gate ran against
/// one representative Phase-1 state per condition bucket while emission iterates over every Phase-1
/// state in it — two packets can share ports and table keys and still hash to different cells.
/// Unknown (tainted/unresolvable) cells count as a match, exactly as in the gate.
bool phasesShareIndexCell(const FinalState *fs1, const FinalState *fs2, cstring soName,
                          const ProgramInfo &programInfo) {
    const auto *soReg2 = soRegisterOf(fs2, soName);
    if (soReg2 == nullptr || !soReg2->wasWritten()) return true;
    const auto p1ReadCell = evalConcreteIndex(fs1, readIndexExpression(fs1, soName), programInfo);
    const auto p2Cells = concreteWriteCells(fs2, soName, programInfo);
    if (!p1ReadCell.has_value() || !p2Cells.has_value()) return true;
    return p2Cells->count(*p1ReadCell) > 0;
}
}  // namespace

std::set<const IR::SymbolicVariable *> StateDependencyTracker::collectIndexSymVars(
    const TestObject *soReg) const {
    return collectIndexSymVarsImpl(soReg);
}

void StateDependencyTracker::pinIndexInputsToPhase1(
    ExecutionState &init, const FinalState *fs1,
    const std::set<const IR::SymbolicVariable *> &symVars) {
    pinIndexInputsToPhase1Impl(init, fs1, symVars);
}

void StateDependencyTracker::pinPacketToPhase1(ExecutionState &init, const FinalState *fs1,
                                               int inputPort,
                                               const IR::Expression *inputPortSymExpr) {
    pinPacketToPhase1Impl(init, fs1, inputPort, inputPortSymExpr);
}

void StateDependencyTracker::concretizeInputPacket(ExecutionState &init, const FinalState *fs,
                                                   int inputPort,
                                                   const IR::Expression *inputPortSymExpr) {
    // Fill BOTH the input packet (so the emitted/replayed test carries real bytes) and the parser
    // buffer with @p fs's concrete packet bytes, so slicePacketBuffer slices CONSTANTS and never
    // mints a fresh symbolic pktvar. With constant operands, Hash.get resolves eagerly to the real
    // CRC during execution — the path follows the real hash branch (fork-free) and CRC-indexed
    // register reads/writes land in the real cell (so the emitted affected_register, ports, and the
    // P1/P2/P3 packets are all mutually consistent).
    const auto &model = fs->getFinalModel();
    const auto *pktConst = model.evaluate(fs->getExecutionState()->getInputPacket(), true);
    const auto *pktSize = model.evaluate(ExecutionState::getInputPacketSizeVar(), true);
    init.appendToInputPacket(pktConst);
    init.appendToPacketBuffer(pktConst);
    init.pushPathConstraint(new IR::Equ(ExecutionState::getInputPacketSizeVar(), pktSize));
    init.pushPathConstraint(
        new IR::Equ(inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, inputPort)));
}

// ---------------------------------------------------------------------------
// --cp-annotation: attacker-port authorization LABELLING (plan T3)
//
// Deliberately label-only: the attacker port is never constrained at generation time. Filtering
// generation would silently bake in "insiders are trusted" and lose the compromised-participant
// case, so the verdict is attached to the emitted test and the triage decision stays explicit.
// File-local (same idiom as pinSinkEntryForLegit) so this stays a .cpp-only change.
// ---------------------------------------------------------------------------
static const CpAnnotation *cpAnnotation() { return loadedCpAnnotation(); }

/// Emit an authorization verdict for a tampering test whose Phase-2 packet entered on
/// @p attackerPort. Silent when no annotation is loaded, so default behaviour is unchanged.
static void labelAttackerPort(const P4StateDependency::DependencyGraphs::SOChain &chain,
                              int attackerPort) {
    const auto *ann = cpAnnotation();
    if (ann == nullptr) return;
    const auto *rule = ann->registerRule(chain.soName);
    cstring verdict;
    cstring why;
    if (rule == nullptr) {
        verdict = "unknown"_cs;
        why = "state object is not annotated"_cs;
    } else if (!rule->partitionedBy.isNullOrEmpty()) {
        verdict = "partitioned"_cs;
        why = "cell is indexed by an unspoofable principal; cross-principal poisoning is "
              "structurally impossible (negative control)"_cs;
    } else if (rule->writableBy.empty()) {
        // Empty writable_by is documented as "unconstrained" (cp_annotation.h). Falling through to
        // the role comparison below would score it "unauthorized", turning every state object with
        // no declared owner into a privilege-escalation candidate - the opposite of the intent.
        verdict = "unconstrained"_cs;
        why = "state object declares no writer role, so no port is privileged over another"_cs;
    } else if (!ann->hasConcretePorts()) {
        verdict = "unknown"_cs;
        why = "roles are declared abstract (no concrete ports), so the attacker's port cannot be "
              "mapped to a role"_cs;
    } else {
        auto roles = ann->rolesForPort(attackerPort);
        bool ok = false;
        // `shared` means the protocol lets any declared PARTICIPANT write this state, so
        // membership in any declared role suffices. It deliberately does not short-circuit the
        // port check: a writer whose port maps to no declared role is an outsider, and surfacing
        // that is the whole point of the port layer (P4xos's round register is writable by any
        // host on the wire even though only participants are supposed to write it).
        // writable_by is the sole authority. `shared` records that the protocol shares this state
        // among participants, which is a statement about READING it: P4xos's learner state is read
        // by every participant but written only by acceptors, so sharing must not widen write
        // authorization. A participant that is not a named writer is still unauthorized.
        for (const auto &r : rule->writableBy)
            if (roles.count(r) > 0) ok = true;
        verdict = ok ? "authorized"_cs : "unauthorized"_cs;
        why = ok ? "attacker's port maps to a role permitted to write this state object"_cs
                 : "attacker's port maps to no role permitted to write this state object "
                   "(privilege escalation candidate)"_cs;
    }
    printInfo("[Tampering] Port authorization: chain id=%1% SO=%2% attacker_port=%3% verdict=%4% "
              "(%5%)",
              chain.id, chain.soName, attackerPort, verdict, why);
}

/// Compare a control-plane name against a clause name, tolerating qualification: annotations are
/// written with the source-level table/action name ("drop_tbl", "handle_2a") while the executing
/// IR uses control-plane names ("Ingress.drop_tbl").
static bool cpNameMatches(cstring full, cstring want) {
    if (full == want) return true;
    const auto tail = [](cstring c) {
        const std::string s(c.string_view());
        auto p = s.find_last_of('.');
        return p == std::string::npos ? s : s.substr(p + 1);
    };
    return tail(full) == tail(want);
}

/// A term whose op reads `value` is enforceable only if the file really carried one. CpTerm::value
/// defaults to 0 and --dump-cp-stubs writes skeletons with "value": null, so without this an
/// unfilled stub would read as "== 0" and prune (or admit) tests on a constraint nobody wrote.
static bool cpTermValueIsUsable(const CpTerm &term) {
    switch (term.op) {
        case CpTerm::Op::Eq:
        case CpTerm::Op::Neq:
        case CpTerm::Op::Lpm:
        case CpTerm::Op::Ternary:
            return term.hasValue;
        default:
            return true;
    }
}

/// True when this terminal's synthesized table entries contradict a declared control-plane
/// assumption, i.e. the real controller would never install this configuration, so the test is
/// not realizable. Used to DROP the test before emission (plan T4).
///
/// Under-constrains by design: only the enforceable clause kinds are checked, a table with no
/// clause is unconstrained, and a table whose config cannot be evaluated is left alone. A wrong
/// assumption here costs a MISSED bug (unsound), which is worse than a false positive.
/// Does @p term hold for the concrete key value of @p match in an emitted entry?
///
/// The plan called for symbolic terms to become path constraints at generation time. They are
/// evaluated here against the concretised entry instead: `ControlPlaneState::getTableKey` needs the
/// key's IR type, which is not reachable at the Phase-2 init site without threading table IR
/// through several layers. Post-hoc evaluation prunes exactly the same tests - it only forfeits the
/// ability to STEER the search toward satisfying entries, which is a search-efficiency property,
/// not a correctness one. Generation-time steering can be layered on later.
static bool cpTermHolds(const CpTerm &term, const TableMatch *match) {
    // An unfilled value makes the term say nothing; "says nothing" is a guard that does not hold,
    // which leaves the test alone - the safe direction for a prune (see the header comment).
    if (!cpTermValueIsUsable(term)) return false;
    // Read the key's concrete value whichever match kind the entry used.
    const IR::Constant *val = nullptr;
    const IR::Constant *mask = nullptr;
    const IR::Constant *plen = nullptr;
    if (const auto *ex = match->to<Exact>(); ex != nullptr) {
        val = ex->getEvaluatedValue();
    } else if (const auto *tern = match->to<Ternary>(); tern != nullptr) {
        val = tern->getEvaluatedValue();
        mask = tern->getEvaluatedMask();
    } else if (const auto *lpm = match->to<LPM>(); lpm != nullptr) {
        val = lpm->getEvaluatedValue();
        plen = lpm->getEvaluatedPrefixLength();
    }
    if (val == nullptr) return false;
    const big_int key = val->value;
    const int width = val->type != nullptr ? val->type->width_bits() : 0;
    // Explicit return type: boost expression templates otherwise deduce two different types here.
    const auto prefixMask = [&](int len) -> big_int {
        if (width <= 0 || len < 0 || len > width) return big_int(0);
        big_int m = 0;
        for (int i = 0; i < len; ++i) m = (m << 1) | 1;
        return big_int(m << (width - len));
    };
    switch (term.op) {
        case CpTerm::Op::Eq:
            return key == term.value;
        case CpTerm::Op::Neq:
            return key != term.value;
        case CpTerm::Op::In:
            return std::find(term.values.begin(), term.values.end(), key) != term.values.end();
        case CpTerm::Op::Range:
            return key >= term.lo && key <= term.hi;
        case CpTerm::Op::Lpm: {
            // Compare under the ANNOTATION's prefix; if the entry is itself an LPM match, it must
            // be at least as specific, otherwise it covers addresses the clause does not describe.
            const big_int m = prefixMask(term.prefix);
            if (m == 0) return false;
            if (plen != nullptr && static_cast<int>(plen->value) < term.prefix) return false;
            return (key & m) == (term.value & m);
        }
        case CpTerm::Op::Ternary: {
            const big_int m = mask != nullptr ? mask->value : term.mask;
            if (m == 0) return false;
            return (key & m) == (term.value & m);
        }
        case CpTerm::Op::Unsupported:
        default:
            return false;  // never approximate an op we do not understand
    }
}

/// cpTermHolds for a term that constrains action data rather than a key. Action arguments carry no
/// mask or prefix, so only the value-comparison ops are meaningful; Lpm/Ternary against action data
/// is a malformed annotation and must constrain nothing rather than be approximated.
static bool cpActionDataTermHolds(const CpTerm &term, const big_int &value) {
    if (!cpTermValueIsUsable(term)) return false;
    switch (term.op) {
        case CpTerm::Op::Eq:
            return value == term.value;
        case CpTerm::Op::Neq:
            return value != term.value;
        case CpTerm::Op::In:
            return std::find(term.values.begin(), term.values.end(), value) != term.values.end();
        case CpTerm::Op::Range:
            return value >= term.lo && value <= term.hi;
        case CpTerm::Op::Lpm:
        case CpTerm::Op::Ternary:
        case CpTerm::Op::Unsupported:
        default:
            return false;
    }
}

/// True when @p table's `const entries` exhaustively cover its key space, i.e. every reachable key
/// combination is named by an entry, so the table can NEVER miss.
///
/// Such a table's key->action map is fixed in the program text: it is a deterministic selector, not
/// a control-plane-programmable table, and a HIT->MISS tamper against it is not merely unlikely but
/// structurally impossible. linkguardian's `era_correction` is the canonical shape -- two 1-bit
/// exact keys with four const entries covering (0,0) (0,1) (1,0) (1,1) -- and every h2m case emitted
/// against it is a false positive.
///
/// Deliberately conservative: it answers true only for all-exact keys with constant entry values and
/// a fully enumerated product. Anything else (ternary/lpm/range keys, a non-constant entry value, a
/// key space too large to enumerate) returns false and leaves behaviour exactly as before, because a
/// wrong "cannot miss" would silently delete real findings.
static bool constEntriesCoverKeySpace(const IR::P4Table *table) {
    if (table == nullptr) return false;
    const auto *entries = table->getEntries();
    const auto *key = table->getKey();
    if (entries == nullptr || key == nullptr || entries->entries.empty()) return false;

    // Per-key bit widths, and the size of the whole key space. Capped: coverage is decided by
    // enumerating the space, so a wide key must bail rather than loop forever -- and a space that
    // large cannot be covered by an entry list anyway.
    constexpr size_t kMaxKeySpace = 1u << 16u;
    std::vector<int> widths;
    size_t keySpace = 1;
    for (const auto *ke : key->keyElements) {
        if (ke->matchType == nullptr || ke->matchType->path == nullptr) return false;
        const cstring matchKind = ke->matchType->path->name.name;
        // exact and ternary are enumerable as cubes. lpm/range/optional are not handled here and
        // must fall through to the existing behaviour.
        if (matchKind != P4Constants::MATCH_KIND_EXACT &&
            matchKind != P4Constants::MATCH_KIND_TERNARY)
            return false;
        const auto *bits = ke->expression->type->to<IR::Type_Bits>();
        if (bits == nullptr || bits->width_bits() <= 0 || bits->width_bits() >= 32) return false;
        const size_t domain = static_cast<size_t>(1) << bits->width_bits();
        if (domain > kMaxKeySpace / keySpace) return false;  // too large to enumerate
        widths.push_back(bits->width_bits());
        keySpace *= domain;
    }
    if (widths.empty()) return false;

    // Each const entry is a cube: (value, mask) per key, where mask 0 means "don't care". A ternary
    // entry writes `v &&& m` (IR::Mask) or `_` (IR::DefaultExpression); an exact one is a bare
    // constant, i.e. an all-ones mask.
    struct Cube {
        std::vector<big_int> value;
        std::vector<big_int> mask;
    };
    std::vector<Cube> cubes;
    for (const auto *entry : entries->entries) {
        if (entry->keys == nullptr || entry->keys->components.size() != widths.size()) return false;
        Cube cube;
        for (size_t i = 0; i < widths.size(); ++i) {
            const IR::Expression *k = entry->keys->components.at(i);
            const big_int allOnes = (big_int(1) << widths[i]) - 1;
            if (const auto *c = k->to<IR::Constant>()) {
                cube.value.push_back(c->value & allOnes);
                cube.mask.push_back(allOnes);
            } else if (k->is<IR::DefaultExpression>()) {
                cube.value.push_back(0);
                cube.mask.push_back(0);  // don't care
            } else if (const auto *m = k->to<IR::Mask>()) {
                const auto *mv = m->left->to<IR::Constant>();
                const auto *mm = m->right->to<IR::Constant>();
                if (mv == nullptr || mm == nullptr) return false;
                cube.value.push_back(mv->value & allOnes);
                cube.mask.push_back(mm->value & allOnes);
            } else {
                return false;  // range/other -> not decided here
            }
        }
        cubes.push_back(std::move(cube));
    }

    // Enumerate the key space and require every point to be matched by some cube. Direct rather than
    // clever: the space is capped at 64K points and this runs once per chain setup.
    for (size_t point = 0; point < keySpace; ++point) {
        size_t rest = point;
        std::vector<big_int> coords;
        coords.reserve(widths.size());
        for (size_t i = widths.size(); i-- > 0;) {
            const size_t domain = static_cast<size_t>(1) << widths[i];
            coords.push_back(big_int(rest % domain));
            rest /= domain;
        }
        std::reverse(coords.begin(), coords.end());
        bool matched = false;
        for (const auto &cube : cubes) {
            bool all = true;
            for (size_t i = 0; i < widths.size(); ++i) {
                if ((coords[i] & cube.mask[i]) != (cube.value[i] & cube.mask[i])) {
                    all = false;
                    break;
                }
            }
            if (all) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// --dump-cp-stubs: per-sink control-plane report.
//
// The annotation gap is invisible by default: an un-annotated table's action data is free-symbolic,
// the solver picks whatever satisfies the path, and nothing says a threshold was ever involved. That
// is how SketchLib's countmin emitted `threshold = 0` tests for months. This pass names, per sink,
// exactly what an annotation COULD pin and hands back a skeleton to paste.
//
// Report-only. It records what generation saw; it never changes what generation does.
// ---------------------------------------------------------------------------
namespace {

struct SinkStub {
    cstring table;
    bool hasConstEntries = false;
    bool constEntriesCoverAll = false;
    bool constEntryActionsDiffer = false;
    cstring defaultAction;
    std::vector<cstring> actions;
    std::vector<cstring> divergingActions;   // observably different from the default action
    std::vector<std::pair<cstring, cstring>> actionData;  // (action, parameter)
    bool annotated = false;                  // an assume clause already names this table
    bool isSink = false;                     // a chain's sink, vs merely annotatable
};

/// Program-wide action map, built once. An ActionListElement only NAMES an action, so both the
/// const-entry comparison and the stub report need this to reach a body.
const std::unordered_map<cstring, const IR::P4Action *> &programActions(const IR::P4Program *program) {
    static std::unordered_map<cstring, const IR::P4Action *> actions;
    static bool built = false;
    if (!built && program != nullptr) {
        built = true;
        ActionCollector ac(actions);
        program->apply(ac);
    }
    return actions;
}

/// Resolver over programActions, matching bare or control-plane names.
ActionResolver actionResolverFor(const IR::P4Program *program) {
    const auto &actions = programActions(program);
    return [&actions](cstring name) -> const IR::P4Action * {
        auto it = actions.find(name);
        if (it != actions.end()) return it->second;
        return lookupActionBySuffix(actions, name);
    };
}

std::map<cstring, SinkStub> &cpStubRegistry() {
    static std::map<cstring, SinkStub> registry;
    return registry;
}

/// Record @p table under its control-plane name @p cpName. Idempotent: a sink shared by several
/// chains is described once.
void recordSinkStub(const IR::P4Table *table, cstring cpName, const IR::P4Program *program,
                    bool isSink) {
    if (!SymbexOptions::get().cpStubsPath.has_value()) return;
    if (table == nullptr || cpName.isNullOrEmpty()) return;
    auto &registry = cpStubRegistry();
    if (auto it = registry.find(cpName); it != registry.end()) {
        it->second.isSink = it->second.isSink || isSink;  // a table can be recorded both ways
        return;
    }

    SinkStub stub;
    stub.table = cpName;
    const auto *entries = table->getEntries();
    stub.hasConstEntries = entries != nullptr && !entries->entries.empty();
    stub.constEntriesCoverAll = constEntriesCoverKeySpace(table);
    const auto resolve = actionResolverFor(program);
    if (stub.hasConstEntries) stub.constEntryActionsDiffer = constEntryActionsDiverge(table, resolve);

    // Default action and its P4Action, for the divergence comparison below.
    const IR::P4Action *defAction = nullptr;
    std::map<cstring, const IR::Expression *> defBinding;
    if (const auto *defExpr = table->getDefaultAction()) {
        if (const auto *defMce = defExpr->to<IR::MethodCallExpression>()) {
            if (const auto *p = defMce->method->to<IR::PathExpression>()) {
                defAction = resolve(p->path->name.name);
                if (defAction != nullptr) {
                    stub.defaultAction = defAction->controlPlaneName();
                    const auto &params = defAction->parameters->parameters;
                    const auto *args = defMce->arguments;
                    if (args != nullptr && params.size() == args->size())
                        for (size_t i = 0; i < params.size(); ++i)
                            defBinding[params.at(i)->name.name] = args->at(i)->expression;
                }
            }
        }
    }

    if (const auto *al = table->getActionList()) {
        for (const auto *ale : al->actionList) {
            const auto *mce = ale->expression->to<IR::MethodCallExpression>();
            const auto *p = mce != nullptr ? mce->method->to<IR::PathExpression>() : nullptr;
            const auto *action = p != nullptr ? resolve(p->path->name.name) : nullptr;
            if (action == nullptr) continue;
            const cstring name = action->controlPlaneName();
            stub.actions.push_back(name);
            // Action parameters are what an `action_data` clause can pin -- the Step-2 mechanism.
            for (const auto *param : action->parameters->parameters)
                stub.actionData.emplace_back(name, param->controlPlaneName());
            // Does choosing this action instead of the default change anything observable? Uses the
            // same comparison the emission gate uses, so the report agrees with generation.
            if (defAction != nullptr && action != defAction &&
                outcomesDiverge(action, {}, defAction, defBinding))
                stub.divergingActions.push_back(name);
        }
    }

    stub.isSink = isSink;
    if (const auto *ann = cpAnnotation(); ann != nullptr) stub.annotated = !ann->clausesFor(cpName).empty();
    registry[cpName] = std::move(stub);
}

void writeCpStubs() {
    const auto &path = SymbexOptions::get().cpStubsPath;
    if (!path.has_value()) return;
    const auto &registry = cpStubRegistry();
    std::ofstream out(*path);
    if (!out) {
        ::P4::error("Could not open --dump-cp-stubs file %1% for writing", *path);
        return;
    }
    const auto quote = [](cstring s) {
        return std::string("\"") + std::string(s.string_view()) + "\"";
    };
    const auto list = [&](const std::vector<cstring> &v) {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + quote(v[i]);
        return s + "]";
    };
    out << "{\n  \"sinks\": [\n";
    bool firstSink = true;
    for (const auto &[name, s] : registry) {
        if (!firstSink) out << ",\n";
        firstSink = false;
        out << "    {\n";
        out << "      \"table\": " << quote(s.table) << ",\n";
        out << "      \"is_chain_sink\": " << (s.isSink ? "true" : "false") << ",\n";
        out << "      \"already_annotated\": " << (s.annotated ? "true" : "false") << ",\n";
        out << "      \"const_entries\": " << (s.hasConstEntries ? "true" : "false") << ",\n";
        out << "      \"const_entries_cover_key_space\": "
            << (s.constEntriesCoverAll ? "true" : "false") << ",\n";
        out << "      \"const_entry_actions_differ\": "
            << (s.constEntryActionsDiffer ? "true" : "false") << ",\n";
        out << "      \"default_action\": " << quote(s.defaultAction) << ",\n";
        out << "      \"actions\": " << list(s.actions) << ",\n";
        out << "      \"actions_diverging_from_default\": " << list(s.divergingActions) << ",\n";
        out << "      \"action_data_parameters\": [";
        for (size_t i = 0; i < s.actionData.size(); ++i) {
            out << (i ? ", " : "") << "[" << quote(s.actionData[i].first) << ", "
                << quote(s.actionData[i].second) << "]";
        }
        out << "],\n";
        // Paste-ready skeletons. Values are left null on purpose: a generated number would look
        // like evidence. Whoever fills it in must also fill in source/ref/reason.
        out << "      \"assume_skeleton\": [\n";
        bool firstClause = true;
        if (!s.divergingActions.empty()) {
            out << "        {\"table\": " << quote(s.table) << ", \"when\": [], \"then\": {\"action\": "
                << quote(s.divergingActions.front())
                << "}, \"source\": \"FILL-IN\", \"ref\": \"FILL-IN\", \"reason\": \"FILL-IN\"}";
            firstClause = false;
        }
        for (const auto &[act, param] : s.actionData) {
            if (!firstClause) out << ",\n";
            firstClause = false;
            out << "        {\"table\": " << quote(s.table) << ", \"when\": [{\"action_data\": ["
                << quote(act) << ", " << quote(param)
                << "], \"op\": \"eq\", \"value\": null}], \"then\": {\"action\": " << quote(act)
                << "}, \"source\": \"FILL-IN\", \"ref\": \"FILL-IN\", \"reason\": \"FILL-IN\"}";
        }
        out << "\n      ]\n    }";
    }
    out << "\n  ]\n}\n";
    printInfo("[CP stubs] wrote %1% sink(s) to %2%", registry.size(), *path);
}

}  // namespace

static bool violatesCpAssumptions(const FinalState *fs) {
    const auto *ann = cpAnnotation();
    if (ann == nullptr || fs == nullptr) return false;
    const auto *es = fs->getExecutionState();
    for (const auto &[tblName, tblObj] : es->getTestObjectCategory("tableconfigs"_cs)) {
        auto clauses = ann->clausesFor(tblName);
        if (clauses.empty()) continue;
        const auto *cfg = tblObj->evaluate(fs->getFinalModel(), /*doComplete=*/true)->to<TableConfig>();
        if (cfg == nullptr) continue;
        // A default-action override has NO rules -- its action and data sit in a table property --
        // so the old "empty rules ⇒ skip" guard meant a keyless table was never checked against its
        // annotation at all. Take the call from wherever this config actually keeps it; everything
        // below is written against the ActionCall and works unchanged (a default override simply has
        // no match map, which the key terms already treat as "the guard says nothing").
        const bool hasRules = cfg->getRules() != nullptr && !cfg->getRules()->empty();
        const ActionCall *call = nullptr;
        const TableMatchMap *matches = nullptr;
        if (hasRules) {
            call = cfg->getRules()->front().getActionCall();
            matches = cfg->getRules()->front().getMatches();
        } else if (const auto *defProperty =
                       cfg->getProperty("overriden_default_action"_cs, /*checked=*/false)) {
            call = defProperty->to<ActionCall>();
        }
        if (call == nullptr || call->getAction() == nullptr) continue;
        const cstring chosen = call->getAction()->controlPlaneName();
        for (const auto *c : clauses) {
            if (c->kind == CpAssumeClause::Kind::WhenThen) {
                bool guardHolds = true;
                for (const auto &t : c->when) {
                    // An action_data term constrains the entry's action arguments, not its key, so
                    // it is resolved against the ActionCall rather than the match map. Without this
                    // it fell through to `match == nullptr` below and silently disabled the whole
                    // clause -- the schema parsed action_data but nothing ever consumed it.
                    if (!t.actionDataArg.isNullOrEmpty()) {
                        // A term naming a different action says nothing about this entry.
                        if (!t.actionDataAction.isNullOrEmpty() &&
                            !cpNameMatches(chosen, t.actionDataAction)) {
                            guardHolds = false;
                            break;
                        }
                        const auto *args = call->getArgs();
                        const ActionArg *arg = nullptr;
                        if (args != nullptr) {
                            for (const auto &a : *args) {
                                if (cpNameMatches(a.getActionParamName(), t.actionDataArg)) {
                                    arg = &a;
                                    break;
                                }
                            }
                        }
                        const auto *argVal = arg != nullptr ? arg->getEvaluatedValue() : nullptr;
                        if (argVal == nullptr || !cpActionDataTermHolds(t, argVal->value)) {
                            guardHolds = false;
                            break;
                        }
                        continue;
                    }
                    // Key names appear qualified ("ig_md.lock_val") in the match map but are
                    // written bare in annotations, so compare on the trailing component too.
                    const TableMatch *match = nullptr;
                    if (matches != nullptr) {
                        for (const auto &[name, m] : *matches) {
                            if (cpNameMatches(name, t.key)) {
                                match = m;
                                break;
                            }
                        }
                    }
                    if (match == nullptr) {
                        guardHolds = false;  // key absent from this entry -> guard says nothing
                        break;
                    }
                    if (!cpTermHolds(t, match)) {
                        guardHolds = false;
                        break;
                    }
                }
                if (!guardHolds) continue;
                const bool violates =
                    (!c->thenAction.isNullOrEmpty() && !cpNameMatches(chosen, c->thenAction)) ||
                    (!c->thenActionNe.isNullOrEmpty() && cpNameMatches(chosen, c->thenActionNe));
                if (violates) {
                    printInfo(
                        "[Tampering] CP assumption prunes test: table=%1% chose %2% but the "
                        "controller pairs this key with %3% (%4%)",
                        tblName, chosen,
                        c->thenAction.isNullOrEmpty() ? "something other than "_cs + c->thenActionNe
                                                      : c->thenAction,
                        c->ref);
                    return true;
                }
                continue;
            }
            const bool same = cpNameMatches(chosen, c->action);
            const bool bad = (c->kind == CpAssumeClause::Kind::ActionEq && !same) ||
                             (c->kind == CpAssumeClause::Kind::ActionNeq && same);
            if (bad) {
                printInfo("[Tampering] CP assumption prunes test: table=%1% chose %2% but "
                          "annotation says %3% (%4%)",
                          tblName, chosen, c->raw, c->ref);
                return true;
            }
        }
    }
    return false;
}

// Set by legitPhase3Sink around its attack-attribution replay: when true, runSymbolicPhase3 ALSO
// pins Phase 1's SINK-table entry, so a freshly-synthesised sink entry cannot manufacture a HIT/MISS
// unrelated to the register and defeat the check. File-local because both the setter (legitPhase3Sink)
// and the sole reader (runSymbolicPhase3) live in this translation unit.
static bool pinSinkEntryForLegit = false;

const FinalState *StateDependencyTracker::runSymbolicPhase3(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const FinalState *fs1, int inputPort, const IR::Expression *inputPortSymExpr,
    const std::map<cstring, const TestObject *> &carriedRegs, bool keepWriteCoverage) {
    if (keepWriteCoverage) {
        // Analytical drive-register re-validation: keep the REAL Phase-2 write-coverage ACCEPTANCE
        // (allCovered over writeNodes) so the terminal is accepted only if the full (branch-gated)
        // write path is now covered (the carried register was pre-set past the gate). But DISABLE
        // reaching-set pruning — for update chains it would cut the path before a terminal (same
        // reason the condition-replay branch below clears it).
        currentPhase = TamperingPhase::Phase2_Write;
        currentRequiredNodes = buildRequiredNodes(chain);
        reachingSet_.clear();
        reachingSetValid_ = false;
    } else {
        currentPhase = TamperingPhase::Phase3_Read;
        // The pinned-input replay is (almost) deterministic, so accept ANY terminal and check the flip
        // externally via evalSinkFlip (evalCondition for a condition sink, evalSinkHit for a table
        // sink). Requiring read/write-node coverage would reject every terminal for a read-modify-write
        // SO — its RegisterAction body branches are mutually exclusive (count-sketch `if(res==0) data-1
        // else +1`), so allCovered is unsatisfiable and Phase 3 would yield no terminal — and
        // reaching-set pruning would cut the path before a terminal. So: no required nodes for either
        // sink kind. (Same reasoning the condition sink already used; now applied to the table sink so
        // counter accumulation can drive a Write-Key sink across replays.)
        currentRequiredNodes.clear();
        reachingSet_.clear();
        reachingSetValid_ = false;
    }

    auto &phase3Init = initState.clone();
    // Pre-set the (tampered) register state the caller computed.
    for (const auto &[regName, regObj] : carriedRegs)
        phase3Init.addTestObject("registervalues"_cs, regName, regObj);

    // Replay Phase 1's exact input as CONCRETE bytes, so CRC hashes resolve eagerly and the SO
    // register is read/written at the real cell (keeps fs3's affected_register consistent with the
    // emitted P1/P2 packets, and makes the sink-flip search follow the real hash branch).
    concretizeInputPacket(phase3Init, fs1, inputPort, inputPortSymExpr);

    // Cross-phase control-plane consistency: hardware installs ONE table state for all three
    // phases, so Phase 3 must use the SAME table entries/actions Phase 1 did — otherwise it could
    // synthesize a *different* configuration (e.g. a fresh keyed entry) that manufactures a flip
    // unrealizable under that single installed state (the H2S2C false positive: spreadsketch's
    // tbl_select_level synthesised level>0 only in Phase 3). For every non-sink table: skip new
    // synthesis (→ immutable, compile-time default — matches a Phase-1 that took the default) and,
    // where Phase 1 chose an entry, inject it as a pre-existing config so Phase 3 evaluates the
    // same one. The sink table is left to its own handling (the tamper acts on its key).
    std::vector<cstring> phase3SkipTables;
    {
        const auto &model1 = fs1->getFinalModel();
        const auto *es1 = fs1->getExecutionState();
        for (const auto &[tblName, tbl] : tableByName_) {
            // Normally the sink keeps its own entries (the tamper acts on its key). For an
            // attack-attribution legit replay (pinSinkEntry) the sink must ALSO use Phase 1's exact
            // entry, else a freshly-synthesised sink entry could manufacture a HIT/MISS unrelated to
            // the register and defeat the check.
            if (!pinSinkEntryForLegit && tblName == chain.sinkTableControlPlaneName) continue;
            phase3SkipTables.push_back(tblName);
        }
        for (const auto &[tblName, tblObj] : es1->getTestObjectCategory("tableconfigs"_cs)) {
            if (!pinSinkEntryForLegit && tblName == chain.sinkTableControlPlaneName) continue;
            const auto *evalCfg = tblObj->evaluate(model1, /*doComplete=*/true)->to<TableConfig>();
            if (evalCfg != nullptr) {
                phase3Init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
                // Skipping synthesis does not reach a keyless table (its stepper returns before the
                // immutability check), so its default action + data need their own pin.
                injectPinnedDefaultAction(phase3Init, tblName, evalCfg);
            }
        }
    }

    std::vector<const FinalState *> phase3States;
    {
        // Carry registers (isPhase1=false ⇒ no zero-init); the sink uses its own entries. Non-sink
        // tables are pinned to Phase 1's choice (skip synthesis + pre-existing configs above).
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, phase3SkipTables,
                               /*setRegTracking=*/true, /*isPhase1=*/false);
        runPhase(phase3Init, phase3States, 1);
    }
    return phase3States.empty() ? nullptr : phase3States[0];
}

int StateDependencyTracker::legitPhase3Sink(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const FinalState *fs1, int inputPort, const IR::Expression *inputPortSymExpr) {
    // Carry Phase 1's OWN register writes (no attacker Phase-2 write) into the replay.
    std::map<cstring, const TestObject *> p1OwnCarry;
    for (const auto &[regName, regObj] :
         fs1->getExecutionState()->getTestObjectCategory("registervalues"_cs))
        p1OwnCarry[regName] = regObj->evaluateForCarry(fs1->getFinalModel());
    pinSinkEntryForLegit = true;
    const auto *legit = runSymbolicPhase3(chain, initState, fs1, inputPort, inputPortSymExpr,
                                          p1OwnCarry, /*keepWriteCoverage=*/false);
    pinSinkEntryForLegit = false;
    return legit == nullptr ? -1 : evalSinkFlip(legit);
}

const FinalState *StateDependencyTracker::reDeriveConcretePhase(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const FinalState *fs, int inputPort, const IR::Expression *inputPortSymExpr, bool isPhase1,
    const std::map<cstring, const TestObject *> &carriedRegs) {
    // Accept any terminal: the concrete packet makes the path (near-)deterministic and the CRC
    // resolves eagerly, so there is no per-cell fork to steer around.
    currentPhase = TamperingPhase::Phase3_Read;
    currentRequiredNodes.clear();
    reachingSet_.clear();
    reachingSetValid_ = false;

    auto &init = initState.clone();
    // Pre-set carried registers (Phase-2 tamper replay only; empty for the Phase-1 reference).
    for (const auto &[regName, regObj] : carriedRegs)
        init.addTestObject("registervalues"_cs, regName, regObj);

    // Concretize the input packet so Hash.get resolves eagerly to the real CRC (fork-free path).
    concretizeInputPacket(init, fs, inputPort, inputPortSymExpr);

    // Cross-phase control-plane consistency for the tamper replay (mirror runSymbolicPhase3): pin
    // non-sink tables to Phase 1's choice. The Phase-1 reference picks its own state (no pinning).
    std::vector<cstring> skipTables;
    if (!isPhase1) {
        const auto &model = fs->getFinalModel();
        const auto *es1 = fs->getExecutionState();
        for (const auto &[tblName, tbl] : tableByName_) {
            if (tblName == chain.sinkTableControlPlaneName) continue;
            skipTables.push_back(tblName);
        }
        for (const auto &[tblName, tblObj] : es1->getTestObjectCategory("tableconfigs"_cs)) {
            if (tblName == chain.sinkTableControlPlaneName) continue;
            const auto *evalCfg = tblObj->evaluate(model, /*doComplete=*/true)->to<TableConfig>();
            if (evalCfg != nullptr) {
                init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
                // Same keyless-table gap as runSymbolicPhase3: pin the default action + data.
                injectPinnedDefaultAction(init, tblName, evalCfg);
            }
        }
    }

    std::vector<const FinalState *> states;
    {
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, skipTables,
                               /*setRegTracking=*/true, /*isPhase1=*/isPhase1);
        runPhase(init, states, 8);
    }
    // Pick the terminal matching @p fs's disposition (drop / output port); fall back to @p fs.
    bool d0Drop = false;
    int d0Port = -1;
    evalDisposition(fs, d0Drop, d0Port);
    for (const auto *st : states) {
        bool dDrop = false;
        int dPort = -1;
        evalDisposition(st, dDrop, dPort);
        if (dDrop == d0Drop && dPort == d0Port) return st;
    }
    return fs;  // no consistent terminal matched the original disposition — strictly additive fallback
}

// ---------------------------------------------------------------------------
// Multi-packet Phase 2: accumulate by replaying the SAME attacker packet until the sink/condition
// flips. The packet count is data-driven (Z3 feasibility), not a fixed depth.
// ---------------------------------------------------------------------------

const FinalState *StateDependencyTracker::accumulatePhase2Flip(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const FinalState *fs1, int inputPort1, const FinalState *fs2, int inputPort2,
    const IR::Expression *inputPortSymExpr, int p3Target, size_t &outRepeat) {
    // Fold a terminal's register writes into a carry snapshot keyed by register name. Requires the
    // SO register to be present (else the tamper can't propagate).
    auto carryAll = [&](const FinalState *fs,
                        std::map<cstring, const TestObject *> &out) -> bool {
        bool hasSo = false;
        for (const auto &[regName, regObj] :
             fs->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
            out[regName] = regObj->evaluateForCarry(fs->getFinalModel());
            if (regName == chain.soName) hasSo = true;
        }
        return hasSo;
    };
    // The carried SO scalar value, used only for fixpoint detection (nullopt ⇒ rely on the cap).
    auto soValue = [&](const std::map<cstring, const TestObject *> &regs) -> std::optional<big_int> {
        auto it = regs.find(chain.soName);
        if (it == regs.end()) return std::nullopt;
        return it->second->getCarriedScalarValue();
    };

    std::map<cstring, const TestObject *> carried;
    if (!carryAll(fs2, carried)) return nullptr;  // after the 1st send

    const auto cap = static_cast<size_t>(SymbexOptions::get().maxPhase2Packets);
    size_t k = 1;
    const FinalState *fs3 =
        runSymbolicPhase3(chain, initState, fs1, inputPort1, inputPortSymExpr, carried);

    while ((fs3 == nullptr || evalSinkFlip(fs3) != p3Target) && k < cap) {
        // Replay the SAME Phase-2 packet (fs2's pinned input) from the current carried state to
        // apply one more increment; runSymbolicPhase3 pins the input + pre-sets the carried regs.
        const FinalState *fs2Next =
            runSymbolicPhase3(chain, initState, fs2, inputPort2, inputPortSymExpr, carried);
        if (fs2Next == nullptr) break;  // packet no longer reaches a terminal — cannot progress
        std::map<cstring, const TestObject *> nextCarried;
        if (!carryAll(fs2Next, nextCarried)) break;
        // Fixpoint: if a further replay does not move the SO value, no count will flip the sink.
        auto prevVal = soValue(carried);
        auto nextVal = soValue(nextCarried);
        if (prevVal && nextVal && *prevVal == *nextVal) break;
        carried = std::move(nextCarried);
        ++k;
        fs3 = runSymbolicPhase3(chain, initState, fs1, inputPort1, inputPortSymExpr, carried);
    }

    if (fs3 != nullptr && evalSinkFlip(fs3) == p3Target) {
        outRepeat = k;
        return fs3;
    }
    return nullptr;
}

bool StateDependencyTracker::terminalWroteSO(const ExecutionState &es) const {
    if (currentChain == nullptr) return false;
    for (const auto &[regName, regObj] : es.getTestObjectCategory("registervalues"_cs)) {
        if (regName == currentChain->soName) return regObj->wasWritten();
    }
    return false;
}

const FinalState *StateDependencyTracker::driveRegisterPhase2(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    ExecutionState &phase2Init, const FinalState *fs1, int inputPort1,
    const IR::Expression *inputPortSymExpr, int p3Target, const FinalState *&outFs2,
    size_t &outRepeat, big_int &outFlipValue) {
    // Generous cap on the computed packet count: large enough for real counter thresholds
    // (ACC-Turbo ~10001), small enough to reject pathological extrapolations.
    constexpr int64_t kAnalyticalCap = 1000000;

    auto carrySO = [&](const FinalState *fs, std::map<cstring, const TestObject *> &out)
        -> std::optional<big_int> {
        std::optional<big_int> soVal;
        for (const auto &[regName, regObj] :
             fs->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
            const auto *carried = regObj->evaluateForCarry(fs->getFinalModel());
            out[regName] = carried;
            if (regName == chain.soName) soVal = carried->getCarriedScalarValue();
        }
        return soVal;  // nullopt ⇒ SO absent or non-scalar ⇒ caller falls back
    };

    // 1. Priming packet: a single Phase-2 terminal that wrote the SO (accepted via the relaxed
    //    "wrote-SO" criterion, since the branch-gated write path is unreachable in one packet).
    std::vector<const FinalState *> primeOut;
    {
        phase2AcceptWroteSO_ = true;
        currentPhase = TamperingPhase::Phase2_Write;
        currentRequiredNodes = buildRequiredNodes(chain);
        buildReachingSet();
        auto &primeInit = phase2Init.clone();
        runPhase(primeInit, primeOut, 1);
        phase2AcceptWroteSO_ = false;
    }
    if (primeOut.empty()) return nullptr;
    const FinalState *prime = primeOut[0];
    const int ipPrime = getPortPair(prime).first;

    // 2. Measure (base, delta): SO value after one priming write, then after a second replay.
    std::map<cstring, const TestObject *> s1;
    auto v1opt = carrySO(prime, s1);
    if (!v1opt) return nullptr;
    const FinalState *prime2 =
        runSymbolicPhase3(chain, initState, prime, ipPrime, inputPortSymExpr, s1);
    if (prime2 == nullptr) return nullptr;
    std::map<cstring, const TestObject *> s2;
    auto v2opt = carrySO(prime2, s2);
    if (!v2opt) return nullptr;
    const big_int delta = *v2opt - *v1opt;
    if (delta <= 0) {
        // Non-monotone or saturating: no closed-form k exists. CountSketch/UnivMon land here --
        // their RegisterAction is `if (res == 0) data - 1 else data + 1`, so a cell walks up or down
        // depending on a per-packet hash sign bit and replaying one packet does not drive it in a
        // fixed direction. Logged rather than silently falling back, because it is a different
        // limitation from "no threshold to solve against" and needs a different fix (steering the
        // sign bit, not annotating a threshold).
        printInfo("[Tampering] chain id=%1% (%2%): drive-register measured delta=%3% (<= 0) over one "
                  "replay -- register is not a monotone accumulator; falling back.",
                  chain.id, currentChainName, delta);
        return nullptr;
    }
    const big_int base = *v1opt - delta;     // SO value before the priming packet's write

    // 3. Collect candidate read-values from the threshold gates in writeNodes (an IfStatement whose
    //    condition is a relation with a constant operand). For an increasing counter the gate fires
    //    once the register read reaches C (Geq/Equ) or C+1 (Grt). We try both and let the allCovered
    //    re-validation decide, so we needn't perfectly classify the operator/operand order.
    std::vector<big_int> candidates;
    for (const auto &[v, node] : chain.writeNodes) {
        forAllMatching<IR::Operation_Relation>(node, [&](const IR::Operation_Relation *rel) {
            const IR::Constant *c = rel->left->to<IR::Constant>();
            if (c == nullptr) c = rel->right->to<IR::Constant>();
            if (c == nullptr) return;
            // Which constant, from which relation, at which position. A constant harvested here is
            // only a *candidate* threshold: at this level ACC-Turbo's `data > PACKET_THRESHOLD` and
            // SketchLib's `res == 0` sign selector are indistinguishable, and only the allCovered
            // re-validation below tells them apart. Logged so a corpus run can be audited for chains
            // whose "threshold" is not one.
            printInfo("[Tampering] chain id=%1% (%2%): drive-register candidate constant %3% from "
                      "`%4%` at %5%",
                      chain.id, currentChainName, c->value, rel,
                      rel->getSourceInfo().toPositionString());
            candidates.push_back(c->value + 1);  // Grt
            candidates.push_back(c->value);       // Geq / Equ
        });
    }
    // 3b. Control-plane thresholds. A sketch typically compares its counter against a value the
    //     controller supplies as action data (SketchLib: `tbl_get_threshold_act(bit<32> threshold)`,
    //     compared OUTSIDE the RegisterAction as `est = est - threshold` with the sink keyed on the
    //     sign bit), so no constant relation exists in writeNodes at all and the loop above yields
    //     nothing usable. An `assume` clause pinning that argument is the only statement of what the
    //     deployed threshold is, so its value is admitted as a candidate here.
    //
    //     Deliberately NOT filtered to clauses whose table feeds this chain's sink: the connection
    //     is a dataflow one (threshold -> est -> sign bit -> sink key) that the dependency graph does
    //     not record, and the sink table is a different table from the one carrying the threshold. A
    //     wrong candidate is harmless -- it simply fails the allCovered/evalSinkFlip validation below
    //     -- whereas a too-narrow filter silently drops the only workable candidate.
    if (const auto *ann = cpAnnotation(); ann != nullptr) {
        for (const auto &c : ann->assumeClauses()) {
            for (const auto &t : c.when) {
                // hasValue: an unfilled --dump-cp-stubs skeleton ("value": null) carries value 0,
                // which would otherwise enter the candidate list as a threshold of 0/1.
                if (t.op != CpTerm::Op::Eq || t.actionDataArg.isNullOrEmpty() || !t.hasValue)
                    continue;
                printInfo("[Tampering] chain id=%1% (%2%): drive-register control-plane candidate "
                          "%3% from assume action_data(%4%, %5%) on table %6%",
                          chain.id, currentChainName, t.value, t.actionDataAction, t.actionDataArg,
                          c.table);
                candidates.push_back(t.value + 1);  // strictly-above threshold
                candidates.push_back(t.value);      // at-threshold
            }
        }
    }
    if (candidates.empty()) {
        // No constant-relation gate anywhere in the write path, so k is not derivable from the
        // program text. This is the SketchLib shape: the threshold arrives as a control-plane action
        // parameter (tbl_get_threshold_act) and is compared outside the RegisterAction, so wiring
        // this driver into the key path cannot by itself recover such a chain.
        printInfo("[Tampering] chain id=%1% (%2%): drive-register found NO constant-relation gate in "
                  "%3% writeNodes — k is not derivable from the program (threshold likely "
                  "control-plane supplied); falling back.",
                  chain.id, currentChainName, chain.writeNodes.size());
        return nullptr;
    }

    auto ceilDiv = [](const big_int &a, const big_int &b) { return (a + b - 1) / b; };

    // 4. For each candidate read-value, compute the prior-packet count, pre-set the carried SO to the
    //    value the covering packet will read, and RE-RUN the real allCovered DFS to validate.
    for (const auto &vreg : candidates) {
        if (vreg <= base) continue;                       // gate already satisfiable ⇒ not this path
        const big_int kprior = ceilDiv(vreg - base, delta);
        const big_int overrideVal = base + kprior * delta;
        const big_int k = kprior + 1;                     // + the covering packet itself
        if (k <= 1 || k > kAnalyticalCap) continue;

        std::map<cstring, const TestObject *> overrideRegs = s1;
        overrideRegs[chain.soName] = s1[chain.soName]->withCarriedScalarValue(overrideVal);
        const FinalState *fs2real = runSymbolicPhase3(chain, initState, prime, ipPrime,
                                                      inputPortSymExpr, overrideRegs,
                                                      /*keepWriteCoverage=*/true);
        if (fs2real == nullptr) continue;                 // full write path not covered ⇒ try next

        // 5. Phase 3 flip+diverge from the covering packet's carried (tampered) register state.
        std::map<cstring, const TestObject *> afterCover;
        auto coverVal = carrySO(fs2real, afterCover);
        if (!coverVal) continue;
        const FinalState *fs3 =
            runSymbolicPhase3(chain, initState, fs1, inputPort1, inputPortSymExpr, afterCover);
        if (fs3 == nullptr || evalSinkFlip(fs3) != p3Target) continue;

        outFs2 = fs2real;
        outRepeat = static_cast<size_t>(k);
        // The candidate that actually validated: the register value at which the sink flips. This,
        // not the final accumulated value, is what the harness should test against -- the flood
        // stops as soon as it reaches its target and packet loss moves where that lands.
        outFlipValue = vreg;
        printInfo("[Tampering] chain id=%1% (%2%): analytical drive-register k=%3% "
                  "(base=%4% delta=%5% read=%6%) — allCovered re-validated.",
                  chain.id, currentChainName, outRepeat, base, delta, overrideVal);
        return fs3;
    }
    return nullptr;
}

size_t StateDependencyTracker::runTamperingChain(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const std::vector<const FinalState *> &phase1Bucket, const TamperingCallback &callBack,
    size_t maxPerChain, bool missToHit) {
    currentChain = &chain;

    // Resolve the chain's sink: either a table (H2S2K) or an if-condition (H2S2C). Exactly one is
    // set. pickSuccessor / the flip checks use whichever is non-null.
    currentSinkTable_ = nullptr;
    currentSinkCondition = nullptr;
    if (!chain.sinkTableControlPlaneName.isNullOrEmpty()) {
        auto sinkIt = tableByName_.find(chain.sinkTableControlPlaneName);
        if (sinkIt != tableByName_.end()) currentSinkTable_ = sinkIt->second;
    } else if (chain.sinkConditionNode != nullptr) {
        currentSinkCondition = chain.sinkConditionNode->to<IR::IfStatement>();
    }

    // Report-only: describe this sink for --dump-cp-stubs before any direction-specific logic.
    recordSinkStub(currentSinkTable_, chain.sinkTableControlPlaneName,
                   &programInfo.getP4Program(), /*isSink=*/true);

    // H2S2C: condition sinks use a dedicated flow (symbolic Phase-3 flip confirmation for both
    // directions). Delegate before the table-specific logic below.
    if (currentSinkCondition != nullptr) {
        return runConditionChain(chain, initState, phase1Bucket, callBack, maxPerChain, missToHit);
    }
    if (missToHit && currentSinkTable_ == nullptr) return 0;

    // A sink whose const entries cover the whole key space cannot MISS, so the HIT->MISS direction
    // is asking for a state the program cannot reach. Emitting it produces a test that always fails
    // to reproduce (linkguardian's era_correction h2m class). The MISS->HIT direction is dropped for
    // the same reason: its Phase-1 filter keeps only sink-MISS baselines, of which there are none.
    //
    // What is NOT suppressed is a change of ACTION within the const map -- that is a real observable
    // difference, and it is what the Step-4 divergence matrix is for; this only removes the
    // HIT/MISS framing that does not apply to such a table.
    if (constEntriesCoverKeySpace(currentSinkTable_)) {
        // Two quite different situations, worth telling apart in the log: either the const map is a
        // no-op selector (every entry has the same observable effect, so no key movement inside it
        // can matter and the chain is dead), or its entries really do differ and only the HIT/MISS
        // framing is inapplicable -- that residue is what deliberate action enumeration addresses.
        const bool actionsDiffer = constEntryActionsDiverge(
            currentSinkTable_, actionResolverFor(&programInfo.getP4Program()));
        printInfo("[Tampering] chain id=%1% (%2%): sink '%3%' has const entries covering its entire "
                  "key space, so it can never MISS -- skipping the %4% direction. %5%",
                  chain.id, currentChainName, chain.sinkTableControlPlaneName,
                  missToHit ? "MISS->HIT"_cs : "HIT->MISS"_cs,
                  actionsDiffer
                      ? "Its entries DO select observably different actions, so a key movement "
                        "within the const map remains a candidate (needs deliberate action "
                        "enumeration, not a HIT/MISS flip)."_cs
                      : "Its entries are observably identical, so no key movement within the const "
                        "map can change anything either."_cs);
        return 0;
    }

    // Reset the incremental Z3 solver state between directions/chains. A previous Phase-2 DFS +
    // processPhase() callback leaves write-path constraints in p4Assertions; Z3's accumulated
    // heuristics would otherwise degrade this pass's Phase-1 read queries (branches wrongly pruned
    // unsat). checkSat({}) pops all outstanding assertions so Phase 1 starts from a clean slate.
    solver.checkSat({});

    // ---- Phase 1: take this chain's share of the shared read-baseline traversal ----
    // (collectPhase1Terminals already ran one program-wide DFS and bucketed terminals per chain.)
    currentPhase = TamperingPhase::Phase1_Read;
    std::vector<const FinalState *> phase1States(phase1Bucket.begin(), phase1Bucket.end());

    // Direction filter: the shared collector applied no sink/disposition filter (it served both
    // directions and kept drops), so apply them here.
    if (missToHit) {
        // Keep only reached-and-MISS terminals (the flippable baselines).
        phase1States.erase(
            std::remove_if(phase1States.begin(), phase1States.end(),
                           [this](const FinalState *fs) { return evalSinkHit(fs) != 0; }),
            phase1States.end());
    } else {
        // HIT→MISS keeps sink-HIT terminals. The HIT action may itself DROP — e.g. a deny-ACL sink
        // whose hit action is mark_to_drop — so dropped baselines are kept: under the differential
        // oracle a Phase-1 drop is a valid reference (Phase-1 HIT→drop vs tampered Phase-3
        // MISS→forward is an ACL bypass). Only forwarded terminals are subject to the distinct
        // in/out port invariant.
        const bool distinct = SymbexOptions::get().distinctIOPorts;
        const bool hasSink = currentSinkTable_ != nullptr;
        phase1States.erase(
            std::remove_if(phase1States.begin(), phase1States.end(),
                           [this, distinct, hasSink](const FinalState *fs) {
                               if (hasSink && evalSinkHit(fs) != 1) return true;  // sink missed
                               bool dropped = false;
                               int outPort = -1;
                               evalDisposition(fs, dropped, outPort);
                               if (!dropped && distinct) {
                                   const auto *ipExpr = fs->getExecutionState()->get(
                                       programInfo.getTargetInputPortVar());
                                   const auto inPort = IR::getIntFromLiteral(
                                       fs->getFinalModel().evaluate(ipExpr, true));
                                   if (inPort == outPort) return true;
                               }
                               return false;
                           }),
            phase1States.end());
    }
    if (phase1States.empty()) {
        if (!missToHit)
            warning("[Tampering] Phase 1 found no terminal state for chain id=%1%.", chain.id);
        return 0;
    }
    if (missToHit)
        printInfo("[Tampering MISS→HIT] chain id=%1% (%2%): %3% Phase-1 MISS terminal(s)", chain.id,
                  chain.soName, phase1States.size());

    // Build deduplicated PhaseConditions (port pair + table key values) for Phase 1.
    std::vector<PhaseConditions> phase1Conditions;
    std::map<size_t, size_t> phase1StateToCondition;
    const IR::Expression *inputPortSymExpr = nullptr;
    for (size_t i = 0; i < phase1States.size(); ++i) {
        const auto *fs1 = phase1States[i];
        inputPortSymExpr = fs1->getExecutionState()->get(programInfo.getTargetInputPortVar());
        auto cond = buildPhaseCondition(*fs1, programInfo);
        if (!missToHit) {
            // The input port is always concrete. The output port may be -1 for a dropped HIT-baseline
            // (e.g. a deny-ACL sink whose hit action drops); the distinct-port invariant only applies
            // to forwarded baselines.
            BUG_CHECK(cond.inputPort >= 0, "Phase 1 invalid input port %1%", cond.inputPort);
            if (cond.outputPort >= 0 && SymbexOptions::get().distinctIOPorts) {
                BUG_CHECK(cond.inputPort != cond.outputPort,
                          "Phase 1 identical input/output ports %1%", cond.inputPort);
            }
        }
        auto it = std::find(phase1Conditions.begin(), phase1Conditions.end(), cond);
        size_t idx;
        if (it == phase1Conditions.end()) {
            idx = phase1Conditions.size();
            phase1Conditions.push_back(cond);
            if (!missToHit)
                printInfo("[Tampering] Phase 1 chose input_port=%1% output_port=%2%", cond.inputPort,
                          cond.outputPort);
        } else {
            idx = static_cast<size_t>(std::distance(phase1Conditions.begin(), it));
        }
        phase1StateToCondition[i] = idx;
    }

    // ---- Phase 2: write the tampered value (shared by both directions) ----
    currentPhase = TamperingPhase::Phase2_Write;
    currentRequiredNodes = buildRequiredNodes(chain);
    if (currentRequiredNodes.empty()) {
        if (!missToHit) warning("[Tampering] Chain id=%1% has no writeNodes; skipping.", chain.id);
        return 0;
    }
    buildReachingSet();
    if (!missToHit) {
        printInfo("[Tampering] Phase 2 (Write) — %1% required nodes", currentRequiredNodes.size());
        for (const auto *node : currentRequiredNodes)
            printInfo("  [%1%] %2% %3%", node->node_type_name(), node,
                      node->getSourceInfo().toPositionString());
    }

    std::map<size_t, std::vector<const FinalState *>> phase2StateMap;
    // Analytical drive-register results, keyed by the Phase-2 terminal the driver validated. Same
    // role as in runConditionChain: a precomputed Phase-3 flip terminal plus the packet count k,
    // valid only against the representative Phase-1 state it was derived from.
    std::map<const FinalState *, const FinalState *> drivenFs3;
    std::map<const FinalState *, size_t> drivenRepeat;
    std::map<const FinalState *, const FinalState *> drivenFs1;
    /// Register value at which the sink/condition flips, per driven Phase-2 terminal. Emitted as
    /// AffectedRegister.min_value so the harness tests "did the counter cross the flip point"
    /// instead of an exact value it cannot observe.
    std::map<const FinalState *, big_int> drivenFlipValue;
    size_t phase2StateNum = 0;
    for (size_t i = 0; i < phase1Conditions.size(); ++i) {
        const auto &cond1 = phase1Conditions[i];

        // Find a representative Phase 1 state for this condition bucket (used to extract the
        // evaluated TableConfig for size-1 tables below).
        const FinalState *repPhase1State = nullptr;
        for (size_t k = 0; k < phase1States.size(); ++k) {
            if (phase1StateToCondition[k] == i) {
                repPhase1State = phase1States[k];
                break;
            }
        }

        // Identify size-1 tables whose single slot was already consumed by Phase 1. Phase 2 must
        // not synthesize a new (different) entry for these; instead Phase 1's pre-existing entry is
        // injected into phase2Init so Phase 2 evaluates it as HIT/MISS without a second entry.
        std::vector<cstring> size1Tables;
        for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
            auto tblIt = tableByName_.find(tblName);
            if (tblIt == tableByName_.end()) continue;
            const auto *sizeConst = tblIt->second->getSizeProperty();
            if (sizeConst != nullptr && sizeConst->asInt() == 1) {
                size1Tables.push_back(tblName);
                printInfo("[Tampering] Phase 2: size-1 table '%1%': reusing Phase 1 entry "
                          "(no new entry generated)",
                          tblName);
            }
        }

        // Exclude the sink table from Phase 2's synthesized entries (its entries belong to Phase
        // 1/3), plus size-1 tables whose slot is already occupied by Phase 1's entry.
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, chain.sinkTableControlPlaneName,
                               size1Tables);
        auto &phase2Init = initState.clone();

        if (repPhase1State != nullptr) {
            const auto &model1 = repPhase1State->getFinalModel();
            const auto *es1 = repPhase1State->getExecutionState();
            // Inject Phase 1's evaluated TableConfig for each size-1 table so evalTableConstEntries
            // can evaluate a pre-existing entry rather than creating a fresh symbolic one.
            for (const auto &tblName : size1Tables) {
                const auto *tblObj =
                    es1->getTestObject("tableconfigs"_cs, tblName, /*checked=*/false);
                if (tblObj == nullptr) continue;
                const auto *evalCfg =
                    tblObj->evaluate(model1, /*doComplete=*/true)->to<TableConfig>();
                if (evalCfg != nullptr)
                    phase2Init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
            }
            // Carry Phase 1's register writes into Phase 2's initial state: hardware runs Phase 1 →
            // Phase 2 on the same device without clearing registers, so Phase 2 reads what Phase 1
            // wrote. evaluateForCarry folds those writes into the register's initialValue.
            for (const auto &[regName, regObj] :
                 es1->getTestObjectCategory("registervalues"_cs)) {
                const auto *carried = regObj->evaluateForCarry(model1);
                phase2Init.addTestObject("registervalues"_cs, regName, carried);
            }
        }

        // Pin every table whose Phase-1 state was a default-action override to that same action and
        // data. Independent of size1Tables above: a keyless table has no key, so it never appears in
        // cond1.tableKeyMap and no size/skip mechanism reaches it.
        injectPinnedDefaultActions(phase2Init, cond1);

        // Hash/sketch/bloom-indexed SO register (tainted access index): pin Phase 2 to the exact
        // Phase-1 flow so the attacker collides with the victim's bucket (equal hash inputs ⇒ equal
        // bucket), instead of the distinctness NEQ that would move it to a different bucket. The
        // full classification, shared with runConditionChain, lives in applyPhase2IndexPolicy.
        //
        // The index-steering retry (gatePhase2ByIndex below) needs its own root, cloned before the
        // policy so it can take the skipIndexPin variant: Phase 1's hash-operand pin fixes the
        // victim's index fields, which for a Phase-2 path hashing OTHER fields is an unrelated
        // extra constraint that can make the retry UNSAT.
        auto &steerTemplate = phase2Init.clone();
        bool indexIsPacketDerived = false;
        applyPhase2IndexPolicy(phase2Init, repPhase1State, cond1, inputPortSymExpr, size1Tables,
                               chain.soName, tableByName_, /*skipIndexPin=*/false,
                               indexIsPacketDerived);
        bool steerIndexIsPacketDerived = false;
        applyPhase2IndexPolicy(steerTemplate, repPhase1State, cond1, inputPortSymExpr, size1Tables,
                               chain.soName, tableByName_, /*skipIndexPin=*/true,
                               steerIndexIsPacketDerived);
        // Accept a Phase-2 terminal that actually WROTE the SO even if not every writeNode is
        // covered: a read-modify-write RegisterAction whose body branches (e.g. count-sketch
        // `if(res==0) data-1 else data+1`) has mutually-exclusive write nodes, so strict allCovered
        // over writeNodes is unsatisfiable on any single path. terminalWroteSO confirms the tampering
        // write happened; strict allCovered still passes for non-branching writes (switchv2p/countmin
        // unchanged), so this only ADDS the otherwise-rejected RMW SOs.
        // Keep a pristine clone for the analytical drive-register below (runPhase mutates its root).
        auto &driveTemplate = phase2Init.clone();
        phase2AcceptWroteSO_ = true;
        runPhase(phase2Init, phase2StateMap[i], maxPerChain);
        phase2AcceptWroteSO_ = false;

        // Analytical drive-register, mirroring runConditionChain. A key sink gated on an
        // accumulating register only flips once the counter passes a threshold, which one packet
        // cannot do. Until now the driver had a single call site inside runConditionChain, so every
        // key chain fell back to accumulatePhase2Flip and its --max-phase2-packets cap (64) — far
        // below a real counter threshold. driveRegisterPhase2 itself is already sink-agnostic: it
        // judges the flip via evalSinkFlip, which dispatches to evalSinkHit for a table sink.
        bool hasThresholdGate = false;
        for (const auto &[v, node] : chain.writeNodes) {
            forAllMatching<IR::Operation_Relation>(node, [&](const IR::Operation_Relation *rel) {
                if (rel->left->is<IR::Constant>() || rel->right->is<IR::Constant>())
                    hasThresholdGate = true;
            });
            if (hasThresholdGate) break;
        }
        // A control-plane threshold counts as a gate too, even though no constant relation appears
        // in writeNodes: for the SketchLib shape the comparison lives outside the RegisterAction
        // entirely, so this pre-filter would otherwise skip the driver before it could consider the
        // annotated value. Kept as a cheap pre-check that mirrors driveRegisterPhase2's own
        // candidate collection -- the driver still re-derives and validates the value itself.
        if (!hasThresholdGate) {
            if (const auto *ann = cpAnnotation(); ann != nullptr) {
                for (const auto &c : ann->assumeClauses()) {
                    for (const auto &t : c.when) {
                        if (t.op == CpTerm::Op::Eq && !t.actionDataArg.isNullOrEmpty() &&
                            t.hasValue) {
                            hasThresholdGate = true;
                            break;
                        }
                    }
                    if (hasThresholdGate) break;
                }
            }
        }
        const bool singleWriteEmpty = phase2StateMap[i].empty();
        if (repPhase1State != nullptr && (singleWriteEmpty || hasThresholdGate)) {
            const FinalState *fs2real = nullptr;
            size_t kDrive = 0;
            big_int flipValue = 0;
            const FinalState *fs3Drive = driveRegisterPhase2(
                chain, initState, driveTemplate, repPhase1State, cond1.inputPort, inputPortSymExpr,
                /*p3Target=*/missToHit ? 1 : 0, fs2real, kDrive, flipValue);
            // Adopt the accumulation result when the write path was unreachable in one packet, or
            // when it genuinely needs k>1 (the single-packet terminals cannot flip this threshold).
            if (fs3Drive != nullptr && (singleWriteEmpty || kDrive > 1)) {
                if (!singleWriteEmpty) phase2StateMap[i].clear();
                phase2StateMap[i].push_back(fs2real);
                drivenFs3[fs2real] = fs3Drive;
                drivenRepeat[fs2real] = kDrive;
                drivenFs1[fs2real] = repPhase1State;
                drivenFlipValue[fs2real] = flipValue;
            }
        } else if (repPhase1State != nullptr) {
            // Not driven, and deliberately so: single-packet Phase-2 terminals exist AND the write
            // path holds no constant relation for k to be solved against. Logged because silence
            // here is indistinguishable from "the driver ran and bailed", and the two have very
            // different fixes. This is the SketchLib shape — CM_UPDATE's RegisterAction is a bare
            // `register_data = register_data + 1` with no relation at all, and the real threshold
            // is a control-plane action parameter compared outside it — so the corpus count of this
            // line measures how many key chains the driver cannot reach on program text alone.
            printInfo("[Tampering] chain id=%1% (%2%): drive-register NOT attempted — %3% "
                      "single-packet Phase-2 terminal(s) and no constant relation in %4% "
                      "writeNodes; falling back to accumulatePhase2Flip.",
                      chain.id, currentChainName, phase2StateMap[i].size(),
                      chain.writeNodes.size());
        }
        // Index soundness, last in the bucket so the analytical driver's adopted terminal is gated
        // too: a Phase-2 packet that writes a register cell Phase 1 never read cannot tamper with
        // what Phase 3 reads back, however convincing the rest of the scenario looks.
        gatePhase2ByIndex(chain, currentChainName, programInfo, repPhase1State, phase2StateMap[i],
                          steerTemplate,
                          [this, &chain](ExecutionState &init,
                                         std::vector<const FinalState *> &out, size_t cap) {
                              // driveRegisterPhase2 may have run in between, and its Phase-3 replay
                              // leaves currentPhase/currentRequiredNodes/reachingSet_ pointing at
                              // the read path — a retry started from there would accept any
                              // terminal instead of a write-covering one. Restore the Phase-2
                              // context first.
                              currentPhase = TamperingPhase::Phase2_Write;
                              currentRequiredNodes = buildRequiredNodes(chain);
                              buildReachingSet();
                              // Same acceptance rule as the bucket's own DFS above: a branching RMW
                              // write path never covers every writeNode on one path.
                              phase2AcceptWroteSO_ = true;
                              runPhase(init, out, cap);
                              phase2AcceptWroteSO_ = false;
                          },
                          maxPerChain, drivenFs3);
        phase2StateNum += phase2StateMap[i].size();
    }
    if (phase2StateNum == 0) {
        // The write nodes come from the dep graph, so the write exists in control flow. An
        // exhaustive Phase-2 DFS finding no terminal means the write is reachable but not
        // satisfiable in a single packet (its gate depends on a register precondition only a prior
        // packet can set). Under the sound single-packet model this is the correct outcome.
        printInfo("[Tampering] chain id=%1% (%2%): Phase-2 write is in the control flow but not "
                  "satisfiable in a single packet (likely requires a register precondition set by a "
                  "prior packet); skipping (sound).",
                  chain.id, currentChainName);
        return 0;
    }

    // ---- Phase 3 (HIT→MISS): dynamic — the test script replays Phase 1's packet ----
    if (!missToHit) {
        // Log Phase 2 port pairs (deduplicated per Phase-1 condition bucket).
        for (size_t i = 0; i < phase1Conditions.size(); ++i) {
            const auto &cond1 = phase1Conditions[i];
            for (const auto *fs2 : phase2StateMap.at(i)) {
                auto portPair = getPortPair(fs2);
                printInfo("[Tampering] Phase 2 chose input_port=%1% output_port=%2% from Phase 1 "
                          "ports %3%/%4%",
                          portPair.first, portPair.second, cond1.inputPort, cond1.outputPort);
                labelAttackerPort(chain, portPair.first);
            }
        }

        // Round-robin emission across Phase-1 states so the per-chain cap never starves a later
        // Phase-1 read-state: every Phase-1 state contributes one sub-test before any gets a second.
        size_t subTestId = 0;
        std::vector<size_t> cursor(phase1States.size(), 0);
        std::map<size_t, int> legitSinkByP1;  // memoized attack-attribution per Phase-1 state
        bool chainCapHit = false;
        while (!chainCapHit) {
            bool emittedThisRound = false;
            for (size_t i = 0; i < phase1States.size() && !chainCapHit; ++i) {
                auto &fs2List = phase2StateMap.at(phase1StateToCondition.at(i));
                if (cursor[i] >= fs2List.size()) continue;  // Phase-1 state exhausted
                const auto *fs1 = phase1States[i];
                const auto &cond1 = phase1Conditions[phase1StateToCondition.at(i)];
                // Attack-attribution gate (memoized per Phase-1 state): if the victim's OWN Phase-1
                // register write already drives the sink to MISS when Phase 1 is replayed (a monotonic
                // self-set RegisterAction the victim runs), the flip is NOT caused by the attacker —
                // the Phase-2 write is redundant, so drain this bucket. This is the legit-vs-attack
                // differential the single-run sinkActionsDiverge gate cannot see. (legitSink == -1 =
                // no legit terminal → cannot rule out → fall through and emit.)
                auto lsIt = legitSinkByP1.find(i);
                int legitSink =
                    (lsIt != legitSinkByP1.end())
                        ? lsIt->second
                        : (legitSinkByP1[i] =
                               legitPhase3Sink(chain, initState, fs1, cond1.inputPort,
                                               inputPortSymExpr));
                if (legitSink == 0) {
                    printInfo("[Tampering] HIT→MISS chain id=%1%: victim's own Phase-1 write already "
                              "MISSes sink '%2%' on replay (redundant with attacker); skipping.",
                              chain.id, chain.sinkTableControlPlaneName);
                    cursor[i] = fs2List.size();
                    continue;
                }
                const auto *fs2 = fs2List[cursor[i]++];
                // Sink action-divergence gate: a HIT→MISS flip is observable only if the sink's HIT
                // action (Phase 1) and its default (MISS) action write different output state. If
                // they are provably identical the flip changes nothing — drain this Phase-1 bucket
                // and skip. (Sound-toward-emitting; the differential oracle is the final judge.)
                if (!sinkActionsDiverge(fs1, currentSinkTable_, chain.sinkTableControlPlaneName)) {
                    printInfo("[Tampering] HIT→MISS chain id=%1%: sink '%2%' HIT/default actions do "
                              "not diverge; flip is unobservable, skipping.",
                              chain.id, chain.sinkTableControlPlaneName);
                    cursor[i] = fs2List.size();
                    continue;
                }
                emittedThisRound = true;
                // Derive attacker-chosen register values from Phase 2 (on the *unevaluated* register
                // object so symbolic write expressions remain available for constraints).
                std::map<cstring, const TestObject *> attackerRegValues;
                std::map<cstring, cstring> attackerRegSinkTables;
                std::vector<std::pair<const IR::SymbolicVariable *, const IR::Constant *>>
                    phase2ModelOverrides;
                // The attacker-chosen register value must MISS exactly at the sink key the register
                // flows into (sinkKeyName at sinkTableControlPlaneName) when Phase 3 replays Phase 1.
                std::vector<big_int> forbiddenValues;
                const cstring sinkTableJoined = chain.sinkTableControlPlaneName;
                if (!sinkTableJoined.isNullOrEmpty() && !chain.sinkKeyName.isNullOrEmpty()) {
                    auto tblIt = cond1.tableKeyMap.find(sinkTableJoined);
                    if (tblIt != cond1.tableKeyMap.end()) {
                        auto keyIt = tblIt->second.find(chain.sinkKeyName);
                        if (keyIt != tblIt->second.end()) {
                            const auto *reprVal = keyIt->second->getRepresentativeValue();
                            if (reprVal != nullptr) forbiddenValues.push_back(reprVal->value);
                        }
                    }
                }
                bool soFeasible = true;
                // The index concolic bindings have to be in the model BEFORE the fold: this is the
                // call that concretises affected_register.index, and it happens long before the
                // emission backend re-solves the phase.
                const auto &p2FoldModel =
                    modelWithResolvedIndices(fs2, chain.soName, programInfo);
                for (const auto &[regName, regObj] :
                     fs2->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
                    if (regName != chain.soName) continue;
                    auto attackerResult = regObj->withAttackerValues(
                        p2FoldModel, SymbexOptions::get().stateTamperValue, forbiddenValues);
                    const auto *attackerValue = attackerResult.testObject;
                    const auto &overrides = attackerResult.modelOverrides;
                    soFeasible = attackerResult.feasible;
                    attackerRegValues[regName] = attackerValue;
                    if (!sinkTableJoined.isNullOrEmpty()) attackerRegSinkTables[regName] = sinkTableJoined;
                    phase2ModelOverrides.insert(phase2ModelOverrides.end(), overrides.begin(),
                                                overrides.end());
                    for (const auto &[symVar, val] : overrides) {
                        printInfo("[Tampering] Phase 2 register override: register='%1%' symVar='%2%' "
                                  "value=0x%3%",
                                  regName, symVar->label, val->value.str(0, std::ios_base::hex));
                    }
                    if (overrides.empty()) {
                        printInfo("[Tampering] Phase 3 register '%1%': program-fixed write value (not "
                                  "packet-injectable); emitting the value the packet actually writes",
                                  regName);
                    }
                }
                if (attackerRegValues.empty()) {
                    warning("[Tampering] No register matching '%1%' found in Phase 2 state for chain "
                            "id=%2%; skipping.",
                            chain.soName, chain.id);
                    continue;
                }
                // The Phase-2 packet's register write is a program constant that does NOT flip the
                // sink (it equals the Phase-1 HIT key). This packet can't tamper — skip it; the
                // round-robin will try the next Phase-2 packet (which may run a different action
                // writing a flipping value). Emitting it would produce an un-replayable test.
                const auto drivenIt = drivenFs3.find(fs2);
                const bool isDriven = drivenIt != drivenFs3.end();
                if (!soFeasible || isDriven) {
                    // A single write does not flip the sink (its value equals the Phase-1 HIT key).
                    // For a counter/accumulator register the value is not attacker-chosen; replaying
                    // the same Phase-2 packet drives the register away from the HIT key until the sink
                    // MISSes. Try to accumulate; only give up (and let the round-robin try another
                    // packet) if no packet count up to the cap flips it. Mirrors the MISS→HIT path.
                    // A driven fs2 takes this path even when soFeasible: the analytical driver
                    // already validated the flip at k packets, so it must emit through the
                    // accumulated branch (which carries repeat_count) rather than as a single send.
                    auto [ipAcc, opAcc] = getPortPair(fs2);
                    size_t repeat = 1;
                    const FinalState *fs3 = nullptr;
                    if (isDriven) {
                        // The driven result is valid only against the representative Phase-1 state
                        // it was derived from; other Phase-1 states fall through to the next round.
                        if (fs1 != drivenFs1[fs2]) continue;
                        fs3 = drivenIt->second;
                        repeat = drivenRepeat[fs2];
                    } else {
                        fs3 = accumulatePhase2Flip(chain, initState, fs1, cond1.inputPort, fs2,
                                                   ipAcc, inputPortSymExpr, /*p3Target=*/0, repeat);
                    }
                    if (fs3 == nullptr) {
                        printInfo("[Tampering] Phase 2 packet for chain id=%1%: constant register write "
                                  "does not flip sink '%2%' (no accumulation up to cap); trying another "
                                  "Phase-2 packet.",
                                  chain.id, chain.soName);
                        continue;
                    }
                    // Accumulated HIT→MISS flip confirmed on fs3. Emit the real accumulated value from
                    // fs3's SO-register READ: only the Phase-3 read carries index conditions for the
                    // pinned (== Phase-1) cell. A Phase-2 write terminal does not — sourcing from it
                    // emits index 0 and drops the block entirely for some paths (measured).
                    //
                    // The value is therefore the post-Phase-3 one, which the harness (reading after
                    // Phase 2) cannot match exactly. That is why an accumulating register is emitted
                    // with match_kind=AT_LEAST + min_value: the harness tests the flip threshold, not
                    // this value. See TamperingFinalState::attackerRegisterMinValues.
                    std::map<cstring, const TestObject *> accRegValues;
                    std::map<cstring, cstring> accRegSinkTables;
                    accRegSinkTables[chain.soName] = chain.sinkTableControlPlaneName;
                    const auto *fs1c = reDeriveConcretePhase(chain, initState, fs1, cond1.inputPort,
                                                             inputPortSymExpr, /*isPhase1=*/true, {});
                    std::map<cstring, const TestObject *> p1Carry;
                    for (const auto &[regName, regObj] :
                         fs1c->getExecutionState()->getTestObjectCategory("registervalues"_cs))
                        p1Carry[regName] = regObj->evaluateForCarry(fs1c->getFinalModel());
                    const auto *fs2c = reDeriveConcretePhase(chain, initState, fs2, ipAcc,
                                                             inputPortSymExpr, /*isPhase1=*/false,
                                                             p1Carry);
                    // The Phase-2 gate never saw this pair: reDeriveConcretePhase re-ran both
                    // phases with concrete packets and substituted fresh terminals, and fs1 is any
                    // state of the bucket rather than the representative the gate compared against.
                    // Re-check the cells here so a swapped terminal cannot smuggle back the very
                    // unsoundness the gate exists to remove.
                    if (!phasesShareIndexCell(fs1c, fs2c, chain.soName, programInfo)) {
                        printInfo("[Tampering] chain id=%1% (%2%): re-derived Phase-2 packet "
                                  "writes a cell of '%3%' the re-derived Phase 1 does not read; "
                                  "skipping.",
                                  chain.id, currentChainName, chain.soName);
                        continue;
                    }
                    // Same argument for the control plane: the concrete re-derivation re-ran both
                    // phases and could have installed a different default action, a different
                    // entry action, or different action data than Phase 1 recorded.
                    if (!crossPhaseCpAgrees(cond1,
                                            {{"the re-derived Phase 1", fs1c},
                                             {"the re-derived Phase 2", fs2c}},
                                            chain.id, currentChainName)) {
                        continue;
                    }
                    // Single-send: source from the Phase-2 state (also the state emitted as the
                    // Phase-2 packet), which is what the harness reads. Accumulation keeps Phase 3 --
                    // one emitted packet sent k times has no single-execution state holding base+k,
                    // and those cases are checked via AT_LEAST + min_value, not by value.
                    {
                        // The semantic condition is "did the ATTACKER write?", tested on the Phase-2 state
                        // itself, independent of which state supplies the emitted value. A terminal can
                        // VISIT the write statement without the register recording anything -- node
                        // coverage is control flow, not effect -- and the shared-traversal collector
                        // accepts on coverage. Such a case holds no attacker action: its only SO write is
                        // Phase 3's, i.e. the victim's own replay, so it can never diverge.
                        const auto *p2SoReg = fs2c->getExecutionState()->getTestObject(
                            "registervalues"_cs, chain.soName, false);
                        if (p2SoReg == nullptr || !p2SoReg->wasWritten()) {
                            printInfo("[Tampering] chain id=%1% (%2%): Phase-2 terminal recorded no write "
                                      "to '%3%' — no attacker action to emit; skipping.",
                                      chain.id, currentChainName, chain.soName);
                            continue;
                        }
                        const FinalState *regSrc = (repeat > 1) ? fs3 : fs2c;
                        const auto *srcSoReg = regSrc->getExecutionState()->getTestObject(
                            "registervalues"_cs, chain.soName, false);
                        if (srcSoReg == nullptr) continue;
                        accRegValues[chain.soName] =
                            srcSoReg
                                ->withAttackerValues(
                                    modelWithResolvedIndices(regSrc, chain.soName, programInfo),
                                    SymbexOptions::get().stateTamperValue, {})
                                .testObject;
                    }
                    TamperingFinalState tsAcc{*fs1c, *fs2c, false, cond1.inputPort, cond1.outputPort,
                                              ipAcc, opAcc, accRegValues, {}, accRegSinkTables, {}};
                    // AT_LEAST bound for an analytically-driven accumulation: the value at which
                    // the sink flips. The harness cannot observe an exact value here -- it stops
                    // driving the register the moment it reaches its target, and packet loss moves
                    // where that lands -- so crossing the flip point is the real success condition.
                    if (auto fvIt = drivenFlipValue.find(fs2); fvIt != drivenFlipValue.end())
                        tsAcc.attackerRegisterMinValues[chain.soName] = fvIt->second;
                    tsAcc.chainId = chain.id;
                    tsAcc.subTestId = ++subTestId;
                    tsAcc.phase2RepeatCount = repeat;  // HIT→MISS emits hit_phase=1 (missToHit=false)
                    if (repeat > 1)
                        printInfo("[Tampering HIT→MISS] chain id=%1% sub=%2%: accumulation needs %3% "
                                  "Phase-2 packet(s) to flip the sink.",
                                  chain.id, tsAcc.subTestId, repeat);
                    if (int mgid = evalMulticastGroup(fs1); mgid >= 0) {
                        tsAcc.usesMulticast = true;
                        tsAcc.multicastGroupId = mgid;
                    }
                    reportUnserializedDefaultActions(cond1, chain.id, tsAcc.subTestId);
                    // Same guard as runConditionChain: the concolic re-solve in the test backend
                    // can hit an expression the Z3 backend cannot translate (a TaintExpression, or
                    // a SizedVarbit whose actual and declared widths disagree). One un-emittable
                    // candidate must not abort the whole run.
                    try {
                        callBack(tsAcc);
                    } catch (const std::exception &e) {
                        if (SymbexOptions::get().strict) throw;
                        warning("[Tampering HIT→MISS] chain id=%1% sub=%2%: emission failed (%3%); "
                                "skipping.",
                                chain.id, tsAcc.subTestId, e.what());
                    }
                    if (maxPerChain != 0 && subTestId >= maxPerChain) chainCapHit = true;
                    continue;
                }
                auto [ip2, op2] = getPortPair(fs2);

                // Build selective NEQ constraints for Phase 1's re-solve in processPhase, preventing
                // Z3 from reassigning Phase 1's packet fields / control-plane keys to Phase 2's
                // values (which would create conflicting entries or unexpected Phase-1 hits).
                std::vector<const IR::Expression *> p1ExtraConstraints;
                {
                    auto cond2 = buildPhaseCondition(*fs2, programInfo);
                    std::set<cstring> size1Set;
                    for (const auto &[tblName, _km] : cond1.tableKeyMap) {
                        auto tblIt = tableByName_.find(tblName);
                        if (tblIt == tableByName_.end()) continue;
                        const auto *sizeConst = tblIt->second->getSizeProperty();
                        if (sizeConst != nullptr && sizeConst->asInt() == 1) size1Set.insert(tblName);
                    }
                    for (const auto &[tblName, keyMap2] : cond2.tableKeyMap) {
                        if (size1Set.count(tblName) > 0 ||
                            tblName == chain.sinkTableControlPlaneName)
                            continue;
                        for (const auto &[keyName, match2] : keyMap2) {
                            if (cond1.tableKeyMap.count(tblName) > 0) {
                                // Phase 1 HIT this table: prevent the re-solve from picking Phase 2's
                                // control-plane key (conflicting entries). NEQ is the safe default —
                                // different keys let both phases install any actions without conflict.
                                // (A rigorous "pin EQ when the key is register-index-related" pass is
                                // planned; see the EQ/NEQ-completeness plan.)
                                p1ExtraConstraints.push_back(
                                    match2->buildTableKeyNeqConstraint(tblName, keyName));
                            } else {
                                // Phase 1 MISSED this table but Phase 2 hit it: keep Phase 1's packet
                                // from matching Phase 2's entry.
                                auto tblIt = tableByName_.find(tblName);
                                if (tblIt == tableByName_.end()) continue;
                                const auto *tblIR = tblIt->second;
                                if (tblIR->getKey() == nullptr) continue;
                                for (const auto *keyElem : tblIR->getKey()->keyElements) {
                                    const auto *nameAnnot =
                                        keyElem->getAnnotation(IR::Annotation::nameAnnotation);
                                    if (nameAnnot == nullptr || nameAnnot->getName() != keyName)
                                        continue;
                                    const auto stateVar =
                                        ToolsVariables::convertReference(keyElem->expression);
                                    if (!fs1->getExecutionState()->exists(stateVar)) continue;
                                    const auto *pktField = fs1->getExecutionState()->get(stateVar);
                                    const auto *neq =
                                        match2->buildPacketFieldNeqConstraint(pktField);
                                    // Skip if Phase-1's own terminal already violates this NEQ (its
                                    // field value equals Phase-2's match, e.g. a shared
                                    // register-index / metadata field the attacker pins to collide,
                                    // or a don't-care ternary key): forcing it unequal is
                                    // unsatisfiable. Evaluating the constraint under Phase-1's model
                                    // is a target-agnostic trivial-UNSAT test.
                                    const auto *neqLit =
                                        fs1->getFinalModel().evaluate(neq, true)
                                            ->to<IR::BoolLiteral>();
                                    if (neqLit != nullptr && !neqLit->value) continue;
                                    p1ExtraConstraints.push_back(neq);
                                }
                            }
                        }
                    }
                    // Size-1 tables: pin Phase-1's emitted control-plane key to cond1's value V
                    // (the single slot persists across phases; otherwise Phase-1's free re-solve
                    // picks a colliding key — the switchv2p match_gw/to_gw bug).
                    for (const auto &tblName : size1Set) {
                        auto cIt = cond1.tableKeyMap.find(tblName);
                        if (cIt == cond1.tableKeyMap.end()) continue;
                        for (const auto &[keyName, match1] : cIt->second) {
                            p1ExtraConstraints.push_back(
                                match1->buildTableKeyEqConstraint(tblName, keyName));
                        }
                    }
                }

                // HIT->MISS has no symbolic Phase 3, so Phase 1's own HIT configuration is the
                // one the controller would have to have installed.
                if (violatesCpAssumptions(fs1) || violatesCpAssumptions(fs2)) continue;
                // The Phase-2 gate compared against the bucket's representative Phase-1 state; this
                // emission pairs fs2 with an arbitrary state of the same bucket, and two packets
                // can share ports and table keys yet hash to different cells. Re-check that pair.
                if (!phasesShareIndexCell(fs1, fs2, chain.soName, programInfo)) {
                    printInfo("[Tampering] chain id=%1% (%2%): Phase-2 packet writes a cell of "
                              "'%3%' that this Phase-1 packet does not read; skipping.",
                              chain.id, currentChainName, chain.soName);
                    continue;
                }
                // Cross-phase control-plane consistency: one installed default action per table,
                // and one entry per match key, has to serve both emitted phases. Nothing was
                // propagated unless an annotation fixed it, so a difference here is a real
                // contradiction, not a pinning artefact.
                if (!crossPhaseCpAgrees(cond1, {{"Phase 2", fs2}}, chain.id, currentChainName)) {
                    continue;
                }
                // Phase 3 is a dynamic deviation check: the test script replays Phase 1's packet
                // after Phase 2 sets the attacker value; the observable is the sink-table HIT(Phase
                // 1)→MISS(Phase 3) flip (emitted as hit_phase=1 / miss_phase=3).
                TamperingFinalState ts{*fs1, *fs2, false, cond1.inputPort, cond1.outputPort, ip2, op2,
                                       attackerRegValues, phase2ModelOverrides, attackerRegSinkTables,
                                       p1ExtraConstraints};
                ts.chainId = chain.id;
                ts.subTestId = ++subTestId;
                // If Phase 1's forward (the validator's reference) came from multicast, emit a
                // multicast_group hint so the validator installs the group before replay.
                if (int mgid = evalMulticastGroup(fs1); mgid >= 0) {
                    ts.usesMulticast = true;
                    ts.multicastGroupId = mgid;
                }
                reportUnserializedDefaultActions(cond1, chain.id, ts.subTestId);
                // See the accumulated branch above: an un-emittable candidate is skipped, not fatal.
                try {
                    callBack(ts);
                } catch (const std::exception &e) {
                    if (SymbexOptions::get().strict) throw;
                    warning("[Tampering HIT→MISS] chain id=%1% sub=%2%: emission failed (%3%); "
                            "skipping.",
                            chain.id, ts.subTestId, e.what());
                }
                if (maxPerChain != 0 && subTestId >= maxPerChain) chainCapHit = true;
            }
            if (!emittedThisRound) break;  // every Phase-1 state's Phase-2 paths exhausted
        }
        return subTestId;
    }

    // ---- Phase 3 (MISS→HIT): symbolically replay Phase 1 with the tampered register ----
    // Require the sink to flip MISS→HIT AND the packet disposition to change.
    size_t emitted = 0;
    for (size_t i = 0; i < phase1States.size() && emitted < maxPerChain; ++i) {
        const auto *fs1 = phase1States[i];
        const auto &cond1 = phase1Conditions[phase1StateToCondition[i]];
        bool d1Drop = false;
        int d1Port = -1;
        evalDisposition(fs1, d1Drop, d1Port);

        // Attack-attribution: if the victim's OWN Phase-1 write already drives the sink to HIT when
        // Phase 1 is replayed (a self-set RegisterAction), the MISS→HIT flip is self-induced, not
        // attacker-caused, for every Phase-2 packet under this Phase-1 state — skip the whole state.
        // (legitSink == -1 = no legit terminal → cannot rule out → fall through and emit.)
        if (legitPhase3Sink(chain, initState, fs1, cond1.inputPort, inputPortSymExpr) == 1) {
            printInfo("[Tampering] MISS→HIT chain id=%1%: victim's own Phase-1 write already HITs sink "
                      "'%2%' on replay (redundant with attacker); skipping.",
                      chain.id, chain.sinkTableControlPlaneName);
            continue;
        }

        for (const auto *fs2 : phase2StateMap[phase1StateToCondition[i]]) {
            if (emitted >= maxPerChain) break;

            const auto &model2 = fs2->getFinalModel();
            const auto *es2 = fs2->getExecutionState();
            int ip2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetInputPortVar()), true));
            // Drive the sink to flip MISS→HIT: one send if it suffices, else replay the same Phase-2
            // packet (accumulating the register) until the sink HITs. repeat = packet count.
            size_t repeat = 1;
            const FinalState *fs3 = nullptr;
            if (auto drivenIt = drivenFs3.find(fs2); drivenIt != drivenFs3.end()) {
                // Analytical drive-register result: precomputed flip terminal + k, valid only for
                // the representative Phase-1 state it was derived against.
                if (fs1 != drivenFs1[fs2]) continue;
                fs3 = drivenIt->second;
                repeat = drivenRepeat[fs2];
            } else {
                fs3 = accumulatePhase2Flip(chain, initState, fs1, cond1.inputPort, fs2, ip2,
                                           inputPortSymExpr, /*p3Target=*/1, repeat);
            }
            if (fs3 == nullptr) continue;  // no packet count up to the cap flips the sink MISS→HIT
            // Sink action-divergence gate (replaces the old Phase1-vs-Phase3 disposition compare):
            // a confirmed MISS→HIT flip is observable only if the sink's HIT action (now taken in
            // Phase 3) differs in effect from its default (MISS) action (taken in Phase 1). The
            // end-to-end output divergence is the harness's call — we only drop provably-invisible
            // flips here. (Sound-toward-emitting.)
            if (!sinkActionsDiverge(fs3, currentSinkTable_, chain.sinkTableControlPlaneName)) continue;
            // Disposition of fs1 (MISS) vs fs3 (HIT) — informational only, for the case label.
            bool d3Drop = false;
            int d3Port = -1;
            evalDisposition(fs3, d3Drop, d3Port);

            // Compute the informational case label (MISS→HIT × disposition).
            std::string disp;
            if (d1Drop && !d3Drop) {
                disp = "DROP_TO_FWD";
            } else if (!d1Drop && d3Drop) {
                disp = "FWD_TO_DROP";
            } else {
                disp = "FWD_TO_FWD";
            }

            std::map<cstring, const TestObject *> attackerRegValues;

            int op2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetOutputPortVar()), true));
            std::map<cstring, cstring> attackerRegSinkTables;
            attackerRegSinkTables[chain.soName] = chain.sinkTableControlPlaneName;

            // Emit a sink-flip test: Phase 1 (its own disposition is the validator's reference) →
            // Phase 3 (replay with the tampered register). The flip MISS→HIT is symbolically
            // confirmed above; the end-to-end validator observes the actual Phase-1→Phase-3 output
            // divergence (drop/port/bytes), so p4symbex does not emit a predicted phase3_verify.
            // Re-derive the emitted Phase-1/Phase-2 packets with CONCRETE bytes so their CRC hashes
            // resolve eagerly (fork-free) and processPhase re-solves them SAT (a shared-traversal
            // terminal carries an arbitrary hash-branch fork that contradicts the real CRC). Phase-2
            // carries the re-derived Phase-1's registers (same victim) and reuses fs2's own packet
            // bytes, which already hash to the victim's bucket (pinIndexInputsToPhase1 at generation).
            const auto *fs1c = reDeriveConcretePhase(chain, initState, fs1, cond1.inputPort,
                                                     inputPortSymExpr, /*isPhase1=*/true, {});
            std::map<cstring, const TestObject *> p1Carry;
            for (const auto &[regName, regObj] :
                 fs1c->getExecutionState()->getTestObjectCategory("registervalues"_cs))
                p1Carry[regName] = regObj->evaluateForCarry(fs1c->getFinalModel());
            labelAttackerPort(chain, ip2);
            if (violatesCpAssumptions(fs3) || violatesCpAssumptions(fs1) || violatesCpAssumptions(fs2)) continue;
            const auto *fs2c = reDeriveConcretePhase(chain, initState, fs2, ip2, inputPortSymExpr,
                                                     /*isPhase1=*/false, p1Carry);
            // Same re-check as the HIT→MISS paths: the concrete re-derivation substituted terminals
            // the Phase-2 gate never saw, so confirm the emitted pair still shares the cell.
            if (!phasesShareIndexCell(fs1c, fs2c, chain.soName, programInfo)) {
                printInfo("[Tampering] chain id=%1% (%2%): re-derived Phase-2 packet writes a cell "
                          "of '%3%' the re-derived Phase 1 does not read; skipping.",
                          chain.id, currentChainName, chain.soName);
                continue;
            }
            // MISS→HIT emits all three phases, so all three have to agree on every table's default
            // action, entries and action data — one control-plane state is installed for the whole
            // replay.
            if (!crossPhaseCpAgrees(cond1,
                                    {{"the re-derived Phase 1", fs1c},
                                     {"the re-derived Phase 2", fs2c},
                                     {"Phase 3", fs3}},
                                    chain.id, currentChainName)) {
                continue;
            }
            // The harness reads the SO register AFTER Phase 2 and BEFORE Phase 3
            // (tofino_driver.py:704, tampering.py:1514, both "before Phase 3 overwrites it") -- the
            // only correct moment, since Phase 3 is the victim's replay and its write is not the
            // attacker's doing. For a SINGLE-SEND case the value must therefore come from the
            // Phase-2 state, which is also the state whose packet bytes are emitted as Phase 2:
            // one state, one model, index and value together.
            //
            // Accumulation (repeat > 1) keeps Phase 3: its emitted Phase-2 packet is ONE packet the
            // harness sends k times, so no single-execution state holds base+k. Those cases carry
            // match_kind=AT_LEAST + min_value and are checked against the flip threshold, so the
            // value is not load-bearing there.
            //
            // Empty forbidden set: feasibility is always true here; the value is the real Phase-2
            // write, and the symbolic Phase-3 confirmation (evalSinkHit==HIT above) is the gate.
            {
                // The semantic condition is "did the ATTACKER write?", tested on the Phase-2 state
                // itself, independent of which state supplies the emitted value. A terminal can
                // VISIT the write statement without the register recording anything -- node
                // coverage is control flow, not effect -- and the shared-traversal collector
                // accepts on coverage. Such a case holds no attacker action: its only SO write is
                // Phase 3's, i.e. the victim's own replay, so it can never diverge.
                const auto *p2SoReg = fs2c->getExecutionState()->getTestObject(
                    "registervalues"_cs, chain.soName, false);
                if (p2SoReg == nullptr || !p2SoReg->wasWritten()) {
                    printInfo("[Tampering] chain id=%1% (%2%): Phase-2 terminal recorded no write "
                              "to '%3%' — no attacker action to emit; skipping.",
                              chain.id, currentChainName, chain.soName);
                    continue;
                }
                const FinalState *regSrc = (repeat > 1) ? fs3 : fs2c;
                const auto *srcSoReg = regSrc->getExecutionState()->getTestObject(
                    "registervalues"_cs, chain.soName, false);
                if (srcSoReg == nullptr) continue;
                attackerRegValues[chain.soName] =
                    srcSoReg
                        ->withAttackerValues(
                            modelWithResolvedIndices(regSrc, chain.soName, programInfo),
                            SymbexOptions::get().stateTamperValue, {})
                        .testObject;
            }
            TamperingFinalState ts{*fs1c, *fs2c, false,
                                   cond1.inputPort, cond1.outputPort, ip2, op2,
                                   attackerRegValues, {}, attackerRegSinkTables, {}};
            // AT_LEAST bound for an analytically-driven accumulation: the value at which the
            // sink/condition flips. The harness cannot observe an exact value -- it stops driving
            // the register once it reaches its target and packet loss moves where that lands -- so
            // crossing the flip point is the real success condition.
            if (auto fvIt = drivenFlipValue.find(fs2); fvIt != drivenFlipValue.end())
                ts.attackerRegisterMinValues[chain.soName] = fvIt->second;
           
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.phase2RepeatCount = repeat;
            ts.kind = TamperKind::MissToHit;
            ts.caseLabel = cstring("MISS_TO_HIT/" + disp);
            if (repeat > 1)
                printInfo("[Tampering MISS→HIT] chain id=%1% sub=%2%: accumulation needs %3% Phase-2 "
                          "packet(s) to flip the sink.",
                          chain.id, ts.subTestId, repeat);
            // Whichever phase forwards via multicast (Phase 3 on DROP_TO_FWD, Phase 1 on
            // FWD_TO_DROP) needs its group installed; emit the hint so the validator installs it.
            if (int mgid = evalMulticastGroup(fs3); mgid >= 0) {
                ts.usesMulticast = true;
                ts.multicastGroupId = mgid;
            } else if (int mgid1 = evalMulticastGroup(fs1); mgid1 >= 0) {
                ts.usesMulticast = true;
                ts.multicastGroupId = mgid1;
            }
            std::string p1d = d1Drop ? std::string("drop") : ("port=" + std::to_string(d1Port));
            std::string p3d = d3Drop ? std::string("drop") : ("port=" + std::to_string(d3Port));
            printInfo("[Tampering MISS→HIT] chain id=%1% sub=%2%: sink MISS→HIT, %3% (P1 %4%, P3 %5%)",
                      chain.id, ts.subTestId, ts.caseLabel, p1d, p3d);
            reportUnserializedDefaultActions(cond1, chain.id, ts.subTestId);
            // See runTamperingChain's HIT→MISS branch: an un-emittable candidate is skipped.
            try {
                callBack(ts);
            } catch (const std::exception &e) {
                if (SymbexOptions::get().strict) throw;
                warning("[Tampering MISS→HIT] chain id=%1% sub=%2%: emission failed (%3%); "
                        "skipping.",
                        chain.id, ts.subTestId, e.what());
            }
        }
    }
    return emitted;
}

// ---------------------------------------------------------------------------
// H2S2C: condition-flip tampering (register → if-condition)
// ---------------------------------------------------------------------------

size_t StateDependencyTracker::runConditionChain(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const std::vector<const FinalState *> &phase1Bucket, const TamperingCallback &callBack,
    size_t maxPerChain, bool missToHit) {
    // currentChain / currentSinkCondition were set by the caller. Direction: missToHit=false means
    // TRUE→FALSE (Phase-1 condition true, tampered Phase-3 false); missToHit=true means FALSE→TRUE.
    solver.checkSat({});
    const int p1Val = missToHit ? 0 : 1;
    const int p3Target = 1 - p1Val;

    // ---- Phase 1: take this chain's share of the shared traversal, keep cond == p1Val ----
    currentPhase = TamperingPhase::Phase1_Read;
    std::vector<const FinalState *> phase1States(phase1Bucket.begin(), phase1Bucket.end());
    phase1States.erase(
        std::remove_if(phase1States.begin(), phase1States.end(),
                       [this, p1Val](const FinalState *fs) { return evalCondition(fs) != p1Val; }),
        phase1States.end());
    if (phase1States.empty()) return 0;

    // Deduped PhaseConditions (ports + table keys). Conditions allow any disposition (drop or
    // forward), so no forward/distinct invariant checks apply.
    std::vector<PhaseConditions> phase1Conditions;
    std::map<size_t, size_t> phase1StateToCondition;
    const IR::Expression *inputPortSymExpr = nullptr;
    for (size_t i = 0; i < phase1States.size(); ++i) {
        const auto *fs1 = phase1States[i];
        inputPortSymExpr = fs1->getExecutionState()->get(programInfo.getTargetInputPortVar());
        auto cond = buildPhaseCondition(*fs1, programInfo);
        auto it = std::find(phase1Conditions.begin(), phase1Conditions.end(), cond);
        if (it == phase1Conditions.end()) {
            phase1StateToCondition[i] = phase1Conditions.size();
            phase1Conditions.push_back(cond);
        } else {
            phase1StateToCondition[i] = static_cast<size_t>(std::distance(phase1Conditions.begin(),
                                                                          it));
        }
    }

    // ---- Phase 2: write the tampered value (sinkTableControlPlaneName is empty for condition
    // chains, so no sink table is excluded). ----
    currentPhase = TamperingPhase::Phase2_Write;
    currentRequiredNodes = buildRequiredNodes(chain);
    if (currentRequiredNodes.empty()) {
        if (!missToHit) warning("[Tampering] Chain id=%1% has no writeNodes; skipping.", chain.id);
        return 0;
    }
    buildReachingSet();

    std::map<size_t, std::vector<const FinalState *>> phase2StateMap;
    // Analytical drive-register results (Family 1): when a bucket's single-packet write DFS finds
    // nothing, driveRegisterPhase2 may produce a validated covering Phase-2 terminal with a
    // precomputed flip terminal + packet count k, keyed by that fs2.
    std::map<const FinalState *, const FinalState *> drivenFs3;
    std::map<const FinalState *, size_t> drivenRepeat;
    std::map<const FinalState *, const FinalState *> drivenFs1;
    /// Register value at which the sink/condition flips, per driven Phase-2 terminal. Emitted as
    /// AffectedRegister.min_value so the harness tests "did the counter cross the flip point"
    /// instead of an exact value it cannot observe.
    std::map<const FinalState *, big_int> drivenFlipValue;
    size_t phase2StateNum = 0;
    for (size_t i = 0; i < phase1Conditions.size(); ++i) {
        const auto &cond1 = phase1Conditions[i];
        const FinalState *repPhase1State = nullptr;
        for (size_t k = 0; k < phase1States.size(); ++k) {
            if (phase1StateToCondition[k] == i) {
                repPhase1State = phase1States[k];
                break;
            }
        }
        std::vector<cstring> size1Tables;
        for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
            auto tblIt = tableByName_.find(tblName);
            if (tblIt == tableByName_.end()) continue;
            const auto *sizeConst = tblIt->second->getSizeProperty();
            if (sizeConst != nullptr && sizeConst->asInt() == 1) size1Tables.push_back(tblName);
        }
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, chain.sinkTableControlPlaneName,
                               size1Tables);
        auto &phase2Init = initState.clone();
        if (repPhase1State != nullptr) {
            const auto &model1 = repPhase1State->getFinalModel();
            const auto *es1 = repPhase1State->getExecutionState();
            for (const auto &tblName : size1Tables) {
                const auto *tblObj =
                    es1->getTestObject("tableconfigs"_cs, tblName, /*checked=*/false);
                if (tblObj == nullptr) continue;
                const auto *evalCfg = tblObj->evaluate(model1, /*doComplete=*/true)->to<TableConfig>();
                if (evalCfg != nullptr)
                    phase2Init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
            }
            for (const auto &[regName, regObj] :
                 es1->getTestObjectCategory("registervalues"_cs)) {
                const auto *carried = regObj->evaluateForCarry(model1);
                phase2Init.addTestObject("registervalues"_cs, regName, carried);
            }
        }
        // Same cross-phase default-action pin as the key path: every table carrying a default
        // override, keyless tables included (they never show up in cond1.tableKeyMap).
        injectPinnedDefaultActions(phase2Init, cond1);
        // Hash/sketch/bloom-indexed SO register: its access index is tainted, so symbex can't tell
        // which bucket a packet maps to. The attacker packet must COLLIDE with the Phase-1 flow's
        // bucket, so pin Phase-2 to the exact Phase-1 packet (equal hash inputs ⇒ equal bucket)
        // instead of forcing it to differ — the distinctness NEQ would land it in a different bucket
        // (see ACC-Turbo: dst_addr-NEQ moved the attacker off the victim's bloom slot). The full
        // classification, shared with runTamperingChain, lives in applyPhase2IndexPolicy.
        //
        // The index-steering retry (gatePhase2ByIndex below) needs its own root, cloned before the
        // policy so it can take the skipIndexPin variant — see the same comment on the key path.
        auto &steerTemplate = phase2Init.clone();
        bool indexIsPacketDerived = false;
        applyPhase2IndexPolicy(phase2Init, repPhase1State, cond1, inputPortSymExpr, size1Tables,
                               chain.soName, tableByName_, /*skipIndexPin=*/false,
                               indexIsPacketDerived);
        bool steerIndexIsPacketDerived = false;
        applyPhase2IndexPolicy(steerTemplate, repPhase1State, cond1, inputPortSymExpr, size1Tables,
                               chain.soName, tableByName_, /*skipIndexPin=*/true,
                               steerIndexIsPacketDerived);
        // Keep a pristine clone for the analytical drive-register fallback (runPhase mutates its root).
        auto &driveTemplate = phase2Init.clone();
        runPhase(phase2Init, phase2StateMap[i], maxPerChain);
        // A register-threshold chain (SO read gated by `SO op CONST` in writeNodes) can only flip the
        // sink by ACCUMULATING the counter past the constant — a single packet cannot. The single-
        // packet DFS may still return a coverage terminal whose flip holds only on a tainted/completed
        // condition (e.g. a RANDOM-hash-indexed sketch cell), emitting a false positive. So run the
        // analytical driver whenever the DFS found nothing OR the chain has a constant threshold gate;
        // when the driver needs k>1 the single-packet terminals are spurious for this chain — drop them.
        bool hasThresholdGate = false;
        for (const auto &[v, node] : chain.writeNodes) {
            forAllMatching<IR::Operation_Relation>(node, [&](const IR::Operation_Relation *rel) {
                if (rel->left->is<IR::Constant>() || rel->right->is<IR::Constant>())
                    hasThresholdGate = true;
            });
            if (hasThresholdGate) break;
        }
        const bool singleWriteEmpty = phase2StateMap[i].empty();
        if (repPhase1State != nullptr && (singleWriteEmpty || hasThresholdGate)) {
            // Family 1: the single-packet write DFS found nothing (register-value gate unreached in one
            // packet) — or the chain has a threshold gate the single packet only spuriously crossed.
            const FinalState *fs2real = nullptr;
            size_t kDrive = 0;
            big_int flipValue = 0;
            const FinalState *fs3Drive =
                driveRegisterPhase2(chain, initState, driveTemplate, repPhase1State, cond1.inputPort,
                                    inputPortSymExpr, p3Target, fs2real, kDrive, flipValue);
            // Adopt the accumulation result when the write path was unreachable in one packet, or when
            // it genuinely needs k>1 (the single-packet terminals cannot flip this threshold).
            if (fs3Drive != nullptr && (singleWriteEmpty || kDrive > 1)) {
                if (!singleWriteEmpty) phase2StateMap[i].clear();
                phase2StateMap[i].push_back(fs2real);
                drivenFs3[fs2real] = fs3Drive;
                drivenRepeat[fs2real] = kDrive;
                drivenFs1[fs2real] = repPhase1State;
                drivenFlipValue[fs2real] = flipValue;
            }
        }
        // Index soundness, last in the bucket so the analytical driver's adopted terminal is gated
        // too: a Phase-2 packet that writes a register cell Phase 1 never read cannot flip the
        // condition Phase 3 evaluates.
        gatePhase2ByIndex(chain, currentChainName, programInfo, repPhase1State, phase2StateMap[i],
                          steerTemplate,
                          [this, &chain](ExecutionState &init,
                                         std::vector<const FinalState *> &out, size_t cap) {
                              // Restore the Phase-2 context first: driveRegisterPhase2's Phase-3
                              // replay leaves the read path selected — see the key path's note.
                              currentPhase = TamperingPhase::Phase2_Write;
                              currentRequiredNodes = buildRequiredNodes(chain);
                              buildReachingSet();
                              runPhase(init, out, cap);
                          },
                          maxPerChain, drivenFs3);
        phase2StateNum += phase2StateMap[i].size();
    }
    if (phase2StateNum == 0) {
        printInfo("[Tampering H2S2C] chain id=%1% (%2%): Phase-2 write not satisfiable in a single "
                  "packet; skipping (sound).",
                  chain.id, currentChainName);
        return 0;
    }

    // ---- Phase 3: symbolically replay Phase 1 with the tampered register; confirm the condition
    // flips to p3Target AND the two branches diverge in output. ----
    size_t emitted = 0;
    for (size_t i = 0; i < phase1States.size() && emitted < maxPerChain; ++i) {
        const auto *fs1 = phase1States[i];
        const auto &cond1 = phase1Conditions[phase1StateToCondition[i]];
        // Attack-attribution gate (H2S2C analog of the H2S2K gate in runTamperingChain, sharing the
        // legitPhase3Sink member): if the victim's OWN Phase-1 write already drives the condition to
        // the tampered branch when Phase 1 is replayed (a self-set RegisterAction), the flip is
        // self-induced, not attacker-caused, for every Phase-2 packet under this Phase-1 state — skip
        // it. (legit == -1 = no legit terminal -> cannot rule out -> fall through and emit.) For a
        // condition chain evalSinkFlip dispatches to evalCondition and pinSinkEntryForLegit is inert
        // (no sink table, so runSymbolicPhase3 pins every table to Phase 1 regardless).
        if (legitPhase3Sink(chain, initState, fs1, cond1.inputPort, inputPortSymExpr) == p3Target) {
            printInfo("[Tampering H2S2C] chain id=%1%: victim's own Phase-1 write already flips the "
                      "condition to the tampered branch on replay (redundant with attacker); skipping.",
                      chain.id);
            continue;
        }
        for (const auto *fs2 : phase2StateMap[phase1StateToCondition[i]]) {
            if (emitted >= maxPerChain) break;
            const auto &model2 = fs2->getFinalModel();
            const auto *es2 = fs2->getExecutionState();
            if (!sinkConditionDiverges()) continue;  // then/else write the same output (count-indep.)
            int ip2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetInputPortVar()), true));
            // Drive the tamper to flip: one send if that suffices, else replay the same Phase-2
            // packet (accumulating the register) until the condition flips. repeat = packet count.
            size_t repeat = 1;
            const FinalState *fs3 = nullptr;
            if (auto drivenIt = drivenFs3.find(fs2); drivenIt != drivenFs3.end()) {
                // Analytical drive-register result: precomputed flip terminal + k, valid only for the
                // representative Phase-1 state it was derived against.
                if (fs1 != drivenFs1[fs2]) continue;
                fs3 = drivenIt->second;
                repeat = drivenRepeat[fs2];
            } else {
                fs3 = accumulatePhase2Flip(chain, initState, fs1, cond1.inputPort, fs2, ip2,
                                           inputPortSymExpr, p3Target, repeat);
            }
            if (fs3 == nullptr) continue;  // no packet count up to the cap flips the condition

            // Phase 3's read carries the index conditions for the pinned cell; a Phase-2 write
            // terminal does not. The emitted value is therefore post-Phase-3 and is not what the
            // harness reads -- accumulating registers carry AT_LEAST + min_value for that reason.
            // Same rule as the key path: the harness reads after Phase 2, so a single-send case
            // must be emitted from the Phase-2 state. This path has no reDeriveConcretePhase, and
            // fs2 is exactly the state emitted as the Phase-2 packet (see the spec below), so
            // register and packet stay consistent with each other.
            // The semantic condition is "did the ATTACKER write?", tested on the Phase-2 state
            // itself, independent of which state supplies the emitted value. A terminal can
            // VISIT the write statement without the register recording anything -- node
            // coverage is control flow, not effect -- and the shared-traversal collector
            // accepts on coverage. Such a case holds no attacker action: its only SO write is
            // Phase 3's, i.e. the victim's own replay, so it can never diverge.
            const auto *p2SoReg = fs2->getExecutionState()->getTestObject(
                "registervalues"_cs, chain.soName, false);
            if (p2SoReg == nullptr || !p2SoReg->wasWritten()) {
                printInfo("[Tampering] chain id=%1% (%2%): Phase-2 terminal recorded no write "
                          "to '%3%' — no attacker action to emit; skipping.",
                          chain.id, currentChainName, chain.soName);
                continue;
            }
            const FinalState *regSrc = (repeat > 1) ? fs3 : fs2;
            std::map<cstring, const TestObject *> attackerRegValues;
            const auto *fs3SoReg = regSrc->getExecutionState()->getTestObject(
                "registervalues"_cs, chain.soName, false);
            if (fs3SoReg == nullptr) continue;
            const auto *attackerReg =
                fs3SoReg
                    ->withAttackerValues(
                        modelWithResolvedIndices(regSrc, chain.soName, programInfo),
                        SymbexOptions::get().stateTamperValue, {})
                    .testObject;
            attackerRegValues[chain.soName] = attackerReg;

            // Emitted output port for pinning: the concrete value if symbex pinned it, else -1
            // meaning "don't pin / unknown". A tainted egress is NOT a drop — it just means symbex
            // can't determine the port (e.g. a hash-derived egress); -1 keeps the emitter from
            // pinning Equ(egress=TaintExpression, value) (which the Z3 backend cannot translate), and
            // the test backend records it as the unknown sentinel. The differential oracle observes
            // the actual egress at replay.
            auto openOutputPort = [this](const FinalState *fs) -> int {
                const auto *op = fs->getExecutionState()->get(programInfo.getTargetOutputPortVar());
                if (op == nullptr || Taint::hasTaint(op)) return -1;  // unknown — leave open
                return IR::getIntFromLiteral(fs->getFinalModel().evaluate(op, true));
            };
            int p1OutPort = openOutputPort(fs1);
            int p2OutPort = openOutputPort(fs2);
            // Condition sink: no table, so attackerRegisterSinkTables stays empty.
            labelAttackerPort(chain, ip2);
            if (violatesCpAssumptions(fs3) || violatesCpAssumptions(fs1) || violatesCpAssumptions(fs2)) continue;
            // The Phase-2 gate compared against the bucket's representative Phase-1 state; this
            // emission pairs fs2 with an arbitrary state of the same bucket, and two packets can
            // share ports and table keys yet hash to different cells. Re-check the emitted pair.
            if (!phasesShareIndexCell(fs1, fs2, chain.soName, programInfo)) {
                printInfo("[Tampering H2S2C] chain id=%1% (%2%): Phase-2 packet writes a cell of "
                          "'%3%' that this Phase-1 packet does not read; skipping.",
                          chain.id, currentChainName, chain.soName);
                continue;
            }
            // One installed control-plane state has to serve all three phases of the replay.
            if (!crossPhaseCpAgrees(cond1, {{"Phase 2", fs2}, {"Phase 3", fs3}}, chain.id,
                                    currentChainName)) {
                continue;
            }
            TamperingFinalState ts{*fs1, *fs2, false, cond1.inputPort, p1OutPort, ip2, p2OutPort,
                                   attackerRegValues, {}, {}, {}};
            // AT_LEAST bound for an analytically-driven accumulation: the value at which the
            // sink/condition flips. The harness cannot observe an exact value -- it stops driving
            // the register once it reaches its target and packet loss moves where that lands -- so
            // crossing the flip point is the real success condition.
            if (auto fvIt = drivenFlipValue.find(fs2); fvIt != drivenFlipValue.end())
                ts.attackerRegisterMinValues[chain.soName] = fvIt->second;
           
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.phase2RepeatCount = repeat;
            // A condition sink keeps the HIT/MISS kinds so its emitted path tag stays h2m/m2h;
            // see TamperingFinalState::kind. The Cond* kinds exist but are not selected here.
            ts.kind = missToHit ? TamperKind::MissToHit : TamperKind::HitToMiss;
            ts.caseLabel = cstring(missToHit ? "COND_FALSE_TO_TRUE" : "COND_TRUE_TO_FALSE");
            if (repeat > 1)
                printInfo("[Tampering H2S2C] chain id=%1% sub=%2%: accumulation needs %3% Phase-2 "
                          "packet(s) to flip the condition.",
                          chain.id, ts.subTestId, repeat);
            if (int mgid = evalMulticastGroup(fs3); mgid >= 0) {
                ts.usesMulticast = true;
                ts.multicastGroupId = mgid;
            } else if (int mgid1 = evalMulticastGroup(fs1); mgid1 >= 0) {
                ts.usesMulticast = true;
                ts.multicastGroupId = mgid1;
            }
            printInfo("[Tampering H2S2C] chain id=%1% sub=%2%: condition %3% (%4%→%5%)", chain.id,
                      ts.subTestId, ts.caseLabel, p1Val, p3Target);
            reportUnserializedDefaultActions(cond1, chain.id, ts.subTestId);
            // The concolic re-solve in the test backend can still hit a TaintExpression the Z3
            // backend cannot translate; don't let one un-emittable candidate abort the whole run.
            try {
                callBack(ts);
            } catch (const std::exception &e) {
                if (SymbexOptions::get().strict) throw;
                warning("[Tampering H2S2C] chain id=%1% sub=%2%: emission failed (%3%); skipping.",
                        chain.id, ts.subTestId, e.what());
            }
        }
    }
    return emitted;
}

// ---------------------------------------------------------------------------
// Single-phase DFS helper
// ---------------------------------------------------------------------------

void StateDependencyTracker::runPhase(ExecutionState &phaseInit,
                                       std::vector<const FinalState *> &out,
                                       size_t maxStates) {
    unexploredBranches.clear();
    // phaseInit is a caller-owned clone. The caller is responsible for pushing any
    // Z3 path constraints (e.g., port equality/exclusion from Phase 1's symbolic
    // variable) before calling this function.
    runImpl([&out, maxStates, this](const FinalState &fs) -> bool {
        const auto *es = fs.getExecutionState();
        const auto &opts = SymbexOptions::get();

        if (opts.outputPacketOnly &&
            (es->getPacketBufferSize() <= 0 || es->getProperty<bool>("drop"_cs))) {
            return false;
        }

        // distinctIOPorts: always a terminal-state check because the output port is
        // assigned during execution (not a free initial symbolic variable).
        // Skip when either port is tainted — a tainted port means the path does not
        // constrain the port value, so we cannot evaluate distinctness.
        if (opts.distinctIOPorts) {
            const auto *ipExpr = es->get(programInfo.getTargetInputPortVar());
            const auto *opExpr = es->get(programInfo.getTargetOutputPortVar());
            if (!Taint::hasTaint(ipExpr) && !Taint::hasTaint(opExpr)) {
                const auto &model = fs.getFinalModel();
                auto inputPort = IR::getIntFromLiteral(model.evaluate(ipExpr, true));
                auto outputPort = IR::getIntFromLiteral(model.evaluate(opExpr, true));
                if (inputPort == outputPort) {
                    printInfo("[SDTrack DEBUG] Phase1 state rejected by distinctIOPorts: "
                              "in=%1% == out=%2%", inputPort, outputPort);
                    return false;
                }
                printInfo("[SDTrack DEBUG] Phase1 state accepted: in=%1% out=%2%",
                          inputPort, outputPort);
            }
        }

        out.push_back(new FinalState(fs));
        // Returning true tells runImpl to stop (terminate the DFS). When maxStates is 0
        // we never stop early, so the guided DFS keeps backtracking and collects every
        // valid terminal path; otherwise we stop once maxStates paths are collected.
        return maxStates != 0 && out.size() >= maxStates;
    }, phaseInit);
}

// ---------------------------------------------------------------------------
// Guided DFS
// ---------------------------------------------------------------------------

void StateDependencyTracker::buildReachingSet() {
    reachingSet_.clear();
    reachingSetValid_ = false;
    // getCallGraph() BUGs if no DCG was built; --state-dep enables the DCG. Degrade gracefully
    // (legacy steering, no pruning) when it is absent.
    if (!SymbexOptions::get().dcg) return;

    const auto &dcg = programInfo.getCallGraph();
    const auto &dcgNodes = dcg.getNodes();
    const auto &inEdges = dcg.getInEdges();

    std::vector<const IR::Node *> work;
    auto seed = [&](const IR::Node *n) {
        if (n == nullptr || dcgNodes.find(n) == dcgNodes.end()) return;
        if (reachingSet_.insert(n).second) work.push_back(n);
    };

    // Seed from the required nodes that are control vertices in the DCG. RegisterAction-internal
    // nodes (the apply Function / its body statements) are not DCG vertices and need no seed: they
    // are reached transitively once the `.execute()` assignment (which IS a DCG vertex) is reached.
    // IR::Key required nodes are not DCG vertices either; seed from the owning sink table instead.
    for (const auto *req : currentRequiredNodes) {
        if (req == nullptr) continue;
        if (req->is<IR::Key>()) {
            if (currentChain != nullptr) {
                auto it = tableByName_.find(currentChain->sinkTableControlPlaneName);
                if (it != tableByName_.end()) seed(it->second);
            }
            continue;
        }
        seed(req);
    }
    if (reachingSet_.empty()) return;  // no usable seed → keep legacy behaviour

    // Backward BFS over predecessor edges: closure of all nodes that can reach a seed.
    while (!work.empty()) {
        const auto *n = work.back();
        work.pop_back();
        auto it = inEdges.find(n);
        if (it == inEdges.end() || it->second == nullptr) continue;
        for (const auto *pred : *it->second)
            if (reachingSet_.insert(pred).second) work.push_back(pred);
    }
    reachingSetValid_ = true;
}

std::optional<ExecutionStateReference> StateDependencyTracker::pickSuccessor(
    StepResult successors) {
    if (successors->empty()) return std::nullopt;
    if (successors->size() == 1) return successors->at(0).nextState;

    // The IR node a branch is about to execute (nullptr if its next command is not an IR node).
    auto branchNextNode = [](const auto &b) -> const IR::Node * {
        auto cmdOpt = b.nextState.get().getNextCmd();
        if (!cmdOpt.has_value()) return nullptr;
        if (const auto *np = std::get_if<const IR::Node *>(&*cmdOpt)) return *np;
        return nullptr;
    };
    // A branch already covers a required node via local lookahead or its visited set.
    auto hitsRequired = [this](const auto &b) {
        for (const auto *node : b.potentialNodes)
            if (currentRequiredNodes.count(node) != 0U) return true;
        for (const auto *node : b.nextState.get().getVisited())
            if (currentRequiredNodes.count(node) != 0U) return true;
        return false;
    };
    // Conservative viability: a branch is viable unless the reachability oracle proves its next
    // (tracked) control node cannot reach any required node. Untracked/unknown next nodes are kept.
    auto isViable = [&](const auto &b) {
        if (!reachingSetValid_) return true;             // no oracle → never prune (legacy)
        if (hitsRequired(b)) return true;                // already covers a required node
        const auto *next = branchNextNode(b);
        if (next == nullptr) return true;                // non-IR next command → can't reason
        if (reachingSet_.count(next) != 0U) return true;  // transitively reaches the target
        const auto &dcgNodes = programInfo.getCallGraph().getNodes();
        if (dcgNodes.find(next) == dcgNodes.end()) return true;  // untracked node → keep
        return false;                                    // tracked + unreachable → prune
    };

    // A branch that takes the chain's sink table HIT (Write-Key chains, Phase 1). The HIT branch
    // stamps getTableHitVar(sinkTable)=true into its nextState at creation; every other step leaves
    // it unset. Steering onto it ensures the register value actually matches a table entry, which
    // the Phase-1 HIT filter requires (otherwise the lookup is a MISS and the state is discarded).
    auto isSinkHit = [this](const auto &b) {
        if (currentSinkTable_ == nullptr || currentPhase != TamperingPhase::Phase1_Read)
            return false;
        // In the MISS→HIT pass Phase 1 must land on sink-MISS terminals; steering toward the HIT
        // branch (the default behaviour) would defeat that, so suppress the preference there.
        if (seekMiss_) return false;
        // Only steer to the sink HIT once the read-side required nodes (everything except the sink
        // Key itself) are already covered. The sink table is applied unconditionally, including on
        // control paths that bypass the register read; preferring its HIT before the read is
        // covered would divert onto a path that never reads the register.
        const auto &visited = b.nextState.get().getVisited();
        for (const auto *n : currentRequiredNodes)
            if (!n->is<IR::Key>() && visited.count(n) == 0U) return false;
        const auto &sinkHitVar = TableStepper::getTableHitVar(currentSinkTable_);
        const auto *hv = b.nextState.get().get(sinkHitVar);
        return hv != nullptr && hv->template is<IR::BoolLiteral>() &&
               hv->template to<IR::BoolLiteral>()->value;
    };

    // Prefer (1) the sink table's HIT branch, then (2) a branch that already hits a required node
    // (closest), else (3) the first viable branch (transitive steering toward the target chain).
    std::optional<size_t> chosenIdx;
    for (size_t i = 0; i < successors->size(); ++i) {
        if (isSinkHit(successors->at(i))) {
            chosenIdx = i;
            break;
        }
    }
    if (!chosenIdx.has_value()) {
        for (size_t i = 0; i < successors->size(); ++i) {
            if (hitsRequired(successors->at(i))) {
                chosenIdx = i;
                break;
            }
            if (!chosenIdx.has_value() && isViable(successors->at(i))) chosenIdx = i;
        }
    }

    if (chosenIdx.has_value()) {
        auto chosen = successors->at(*chosenIdx);
        successors->erase(successors->begin() + static_cast<ptrdiff_t>(*chosenIdx));
        // Prune non-viable siblings: only branches that can still reach the target are kept on
        // the backtrack stack, so the DFS no longer wanders subtrees that provably cannot.
        for (auto &b : *successors)
            if (isViable(b)) unexploredBranches.push_back(b);
        return chosen.nextState;
    }

    // No viable branch (e.g. genuine dead end, or oracle unavailable): legacy random fallback.
    auto chosen = popRandomBranch(*successors);
    unexploredBranches.insert(unexploredBranches.end(), successors->begin(), successors->end());
    return chosen.nextState;
}

void StateDependencyTracker::runImpl(const Callback &callBack,
                                     ExecutionStateReference executionState) {
    while (true) {
        try {
            if (executionState.get().isTerminal()) {
                if (sharedPhase1) {
                    // Shared Phase-1 pass: bucket this terminal into every chain whose read targets
                    // it covers; stop once all buckets are full or the examine budget is hit.
                    handleSharedTerminal(executionState.get());
                    if (allPhase1BucketsFull() || phase1Examined >= phase1ExamineBudget) return;
                } else if (sharedPhase2) {
                    // Shared Phase-2 write-path prefilter: mark every chain whose write nodes this
                    // terminal covers as reached.
                    handleSharedPhase2Terminal(executionState.get());
                    // Every chain reached -> nothing left to prune; stop.
                    if (allPhase2BucketsFull()) return;
                    // Out of budget. The search did NOT complete, so an unreached chain is UNKNOWN,
                    // not unreachable — pruning here would drop real tests. Record that and let
                    // collectPhase2Terminals prune nothing.
                    if (phase2Examined >= phase2ExamineBudget) {
                        phase2BudgetExhausted = true;
                        return;
                    }
                } else {
                    // Only emit a test when the path covers all required nodes.
                    const auto &visited = executionState.get().getVisited();
                    const auto &es = executionState.get();
                    // Relaxed write acceptance (Phase-2 RMW + driveRegisterPhase2 priming): when the
                    // SO's RegisterAction wrote the SO, excuse uncovered nodes INSIDE that
                    // RegisterAction — its body branches are mutually exclusive (count-sketch
                    // `if(res==0) data-1 else +1`; ACC-Turbo `if(data>10000) …`), so no single path
                    // covers them all. Nodes outside the RegisterAction stay strictly required.
                    const bool soWrote = phase2AcceptWroteSO_ && terminalWroteSO(es);
                    auto nodeCovered = [&](const IR::Node *n) -> bool {
                        // IR::Key nodes are never passed to markVisited; instead check whether the
                        // table that owns this key had its apply() MethodCallStatement visited.
                        if (n->is<IR::Key>()) {
                            return isTableVisited(currentChain->sinkTableControlPlaneName, visited);
                        }
                        if (visited.count(n) > 0) return true;
                        if (soWrote && isInRegisterActionBody(n)) return true;
                        // Under the Cond policy cmd_stepper CLONES the IfStatement (it reduces the
                        // condition for branch stamping), so the original ESG pointer never appears in
                        // `visited` even when the branch was taken. It does stamp a source-position-
                        // keyed condition var when the branch is evaluated; treat that as coverage of
                        // the IfStatement node. (H2S2K does not clone, so the pointer check above hits.)
                        if (const auto *ifs = n->to<IR::IfStatement>()) {
                            // exists() (not get()) — the condition var is only stamped when the branch
                            // is evaluated, and is absent under the Key policy / on unreached paths;
                            // get() would BUG ("var not in symbolic environment") and skip the whole
                            // path, which is exactly what hid every cs_action-executing terminal.
                            return es.exists(CmdStepper::getConditionVar(ifs));
                        }
                        return false;
                    };
                    bool allCovered = std::all_of(currentRequiredNodes.begin(),
                                                  currentRequiredNodes.end(), nodeCovered);
                    // A Phase-2 terminal must actually have WRITTEN the SO, not merely have visited
                    // the write statement. allCovered is a control-flow property (nodes visited); a
                    // path can reach `value = ...` inside a RegisterAction without the register
                    // recording an index/value pair -- measured on countmin, where 10 of 20 MISS->HIT
                    // Phase-2 terminals have wasWritten()==false while the statement was visited.
                    //
                    // Such a terminal carries no attacker action: the emitted affected_register could
                    // only be sourced from Phase 3, i.e. from the VICTIM's own replayed write, and the
                    // resulting case can never diverge. Reject it here rather than emitting a test
                    // that cannot demonstrate tampering.
                    if (allCovered && currentPhase == TamperingPhase::Phase2_Write &&
                        !terminalWroteSO(es)) {
                        printInfo("[SDTrack] Phase-2 terminal rejected: chain=%1% id=%2% SO=%3% "
                                  "write path visited but the register recorded no write "
                                  "(no attacker action to emit).",
                                  currentChainName, currentChain->id, currentChain->soName);
                        allCovered = false;
                    }
                    if (allCovered) {
                        printInfo("[SDTrack] Test found: chain=%1% id=%2% SO=%3% (%4% nodes)",
                                    currentChainName, currentChain->id, currentChain->soName,
                                    currentRequiredNodes.size());
                        for (const auto *node : currentRequiredNodes) {
                            printInfo("  OK  [%1%] %2% %3%",
                                        node->node_type_name(), node,
                                        node->getSourceInfo().toPositionString());
                        }
                        bool terminate = handleTerminalState(callBack, executionState);
                        if (terminate) return;
                    } else {
                        // DEBUG: show which required nodes were not visited so the caller can
                        // distinguish a node-identity mismatch from a DFS coverage failure.
                        printInfo("[SDTrack DEBUG] Terminal state reached but allCovered=false "
                                "(%1% required nodes):", currentRequiredNodes.size());
                        for (const auto *n : currentRequiredNodes) {
                            printInfo("  %1% [%2%] %3% %4%", (nodeCovered(n) ? "OK  " : "MISS"),
                                    n->node_type_name(), n,
                                    n->getSourceInfo().toPositionString());
                        }
                    }
                }  // end non-shared terminal handling
            } else {
                StepResult successors = step(executionState);
                auto next = pickSuccessor(successors);
                if (next.has_value()) {
                    executionState = next.value();
                    continue;
                }
            }
        } catch (SymbexUnimplemented &e) {
            if (SymbexOptions::get().strict) throw;
            warning("Path encountered unimplemented feature. Message: %1%\n", e.what());
        } catch (Util::CompilerBug &e) {
            // Collecting *every* valid path (runPhase with maxStates>1) makes the guided
            // DFS explore far more of the state space than the previous first-match-wins
            // behaviour did. Some of those paths hit modeling gaps in the shared symbex
            // engine (e.g. a tainted Mux of unknown type reaching ExecutionState::set()),
            // which are raised as BUG_CHECK/CompilerBug. A single unmodelable path must not
            // abort the whole multi-path search: log it and backtrack like an unimplemented
            // feature, so the remaining (modelable) paths — including the discriminating
            // write path — are still collected and emitted. --strict re-throws for debugging.
            if (SymbexOptions::get().strict) throw;
            warning("[SDTrack] Skipping path that triggered an internal error during guided "
                    "DFS. Message: %1%\n", e.what());
        } catch (const std::exception &e) {
            // Some paths surface an unmodeled construct as a plain std::exception rather than a
            // Util::CompilerBug — e.g. a std::out_of_range from a vector range-check inside a
            // stepper (observed on multi-field sink-key / sketch-register paths). As above, a
            // single unmodelable path must not abort the whole search: skip it and backtrack so
            // the remaining (modelable) paths are still collected. --strict re-throws for debugging.
            if (SymbexOptions::get().strict) throw;
            warning("[SDTrack] Skipping path that triggered a std::exception during guided "
                    "DFS. Message: %1%\n", e.what());
        }

        // Backtrack (LIFO).
        if (unexploredBranches.empty()) return;
        Util::ScopedTimer chooseBranchTimer("branch_selection");
        executionState = unexploredBranches.back().nextState;
        unexploredBranches.pop_back();
    }
}

}  // namespace P4::P4Tools::Symbex
