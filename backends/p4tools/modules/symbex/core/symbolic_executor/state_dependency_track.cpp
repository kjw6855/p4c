#include "backends/p4tools/modules/symbex/core/symbolic_executor/state_dependency_track.h"

#include <algorithm>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "ir/ir.h"
#include "ir/irutils.h"
#include "ir/solver.h"
#include "lib/error.h"
#include "lib/timer.h"

#include "backends/p4tools/common/control_plane/symbolic_variables.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/variables.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_annotation.h"
#include "backends/p4tools/modules/symbex/core/small_step/cmd_stepper.h"
#include "backends/p4tools/modules/symbex/core/small_step/table_stepper.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/logging.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

struct PhaseConditions {
    int inputPort = -1;
    int outputPort = -1;
    // tableName → keyName → concrete evaluated match (all match kinds)
    std::map<cstring, std::map<cstring, const TableMatch *>> tableKeyMap;

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
        return true;
    }
};

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
    for (const auto &[tblName, tblObj] : es->getTestObjectCategory("tableconfigs"_cs)) {
        const auto *evaluated = tblObj->evaluate(model, /*doComplete=*/true);
        const auto *cfg = evaluated->to<TableConfig>();
        if (cfg == nullptr) continue;
        for (const auto &rule : *cfg->getRules()) {
            for (const auto &[keyName, match] : *rule.getMatches()) {
                cond.tableKeyMap[tblName][keyName] = match;
            }
        }
    }
    return cond;
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

// Replaces references to an action's parameters with their bound argument expressions, so two
// calls of the same action with different action data yield structurally different bodies.
class ActionParamSubstitute : public Transform {
 public:
    explicit ActionParamSubstitute(std::map<cstring, const IR::Expression *> binding)
        : binding_(std::move(binding)) {}
    const IR::Node *postorder(IR::PathExpression *pe) override {
        auto it = binding_.find(pe->path->name.name);
        if (it != binding_.end()) return it->second;
        return pe;
    }

 private:
    std::map<cstring, const IR::Expression *> binding_;
};

// The (parameter-substituted) output-affecting effect of an action body. `ok=false` marks a body
// we cannot summarize (control flow etc.); callers then treat the actions as divergent.
struct ActionEffect {
    bool ok = true;
    std::vector<std::pair<const IR::Expression *, const IR::Expression *>> assigns;  // (lhs, rhs)
    std::vector<const IR::Expression *> calls;  // method/extern calls (e.g. mark_to_drop)
};

void collectEffect(const IR::Statement *stmt, ActionEffect &eff) {
    if (stmt == nullptr) return;
    if (const auto *block = stmt->to<IR::BlockStatement>()) {
        for (const auto *c : block->components) {
            const auto *s = c->to<IR::Statement>();
            if (s == nullptr) {  // a nested declaration we do not model
                eff.ok = false;
                return;
            }
            collectEffect(s, eff);
            if (!eff.ok) return;
        }
        return;
    }
    if (const auto *asg = stmt->to<IR::AssignmentStatement>()) {
        eff.assigns.emplace_back(asg->left, asg->right);
        return;
    }
    if (const auto *mc = stmt->to<IR::MethodCallStatement>()) {
        eff.calls.push_back(mc->methodCall);
        return;
    }
    if (stmt->is<IR::EmptyStatement>()) return;
    // if/switch/return/exit/...: cannot summarize statically -> be conservative.
    eff.ok = false;
}

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

bool exprEquiv(const IR::Expression *a, const IR::Expression *b) {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return a->equiv(*b);
}

bool effectsEqual(const ActionEffect &a, const ActionEffect &b) {
    if (!a.ok || !b.ok) return false;  // unsummarizable -> not provably equal
    if (a.assigns.size() != b.assigns.size() || a.calls.size() != b.calls.size()) return false;
    for (size_t i = 0; i < a.assigns.size(); ++i) {
        if (!exprEquiv(a.assigns[i].first, b.assigns[i].first)) return false;
        if (!exprEquiv(a.assigns[i].second, b.assigns[i].second)) return false;
    }
    for (size_t i = 0; i < a.calls.size(); ++i)
        if (!exprEquiv(a.calls[i], b.calls[i])) return false;
    return true;
}

ActionEffect summarizeAction(const IR::P4Action *action,
                             const std::map<cstring, const IR::Expression *> &binding) {
    ActionEffect eff;
    if (action == nullptr || action->body == nullptr) {
        eff.ok = false;
        return eff;
    }
    ActionParamSubstitute subst(binding);
    const auto *body = action->body->apply(subst)->to<IR::BlockStatement>();
    if (body == nullptr) {
        eff.ok = false;
        return eff;
    }
    collectEffect(body, eff);
    return eff;
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

    // Diverge unless the two action bodies are provably identical in observable effect.
    auto hitEff = summarizeAction(hitCall->getAction(), hitBinding);
    auto defEff = summarizeAction(defAction, defBinding);
    return !effectsEqual(hitEff, defEff);
}

bool StateDependencyTracker::sinkConditionDiverges() const {
    // H2S2C analog of sinkActionsDiverge: a condition flip is observable only if the then-branch and
    // else-branch write different output state. Compare the two branch bodies' effects locally (no
    // params to bind, unlike actions). A null else-branch contributes an empty effect (it runs
    // nothing), which differs from any non-empty then-branch. Sound-toward-emitting.
    if (currentSinkCondition == nullptr) return true;
    ActionEffect thenEff;
    ActionEffect elseEff;
    collectEffect(currentSinkCondition->ifTrue, thenEff);
    if (currentSinkCondition->ifFalse != nullptr)
        collectEffect(currentSinkCondition->ifFalse, elseEff);
    return !effectsEqual(thenEff, elseEff);
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
    // Headline progress metric: on a run that is cut short by a timeout this is the only record of how
    // far the per-chain Phase-2/3 loop actually got. Emitted in every mode so baselines are comparable.
    printInfo("[Tampering] Chains completed: %1%/%2%", chainsCompleted, allChains.size());
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
}  // namespace

std::set<const IR::SymbolicVariable *> StateDependencyTracker::collectIndexSymVars(
    const TestObject *soReg) const {
    IndexSymVarCollector collector;
    if (soReg != nullptr) {
        for (const auto *idx : soReg->getIndexExpressions()) {
            if (idx != nullptr) idx->apply(collector);
        }
    }
    return collector.vars;
}

void StateDependencyTracker::pinIndexInputsToPhase1(
    ExecutionState &init, const FinalState *fs1,
    const std::set<const IR::SymbolicVariable *> &symVars) {
    const auto &model1 = fs1->getFinalModel();
    for (const auto *sv : symVars) {
        init.pushPathConstraint(new IR::Equ(sv, model1.evaluate(sv, true)));
    }
}

void StateDependencyTracker::pinPacketToPhase1(ExecutionState &init, const FinalState *fs1,
                                               int inputPort,
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

static bool violatesCpAssumptions(const FinalState *fs) {
    const auto *ann = cpAnnotation();
    if (ann == nullptr || fs == nullptr) return false;
    const auto *es = fs->getExecutionState();
    for (const auto &[tblName, tblObj] : es->getTestObjectCategory("tableconfigs"_cs)) {
        auto clauses = ann->clausesFor(tblName);
        if (clauses.empty()) continue;
        const auto *cfg = tblObj->evaluate(fs->getFinalModel(), /*doComplete=*/true)->to<TableConfig>();
        if (cfg == nullptr || cfg->getRules() == nullptr || cfg->getRules()->empty()) continue;
        const auto *call = cfg->getRules()->front().getActionCall();
        if (call == nullptr || call->getAction() == nullptr) continue;
        const cstring chosen = call->getAction()->controlPlaneName();
        const auto *matches = cfg->getRules()->front().getMatches();
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
            if (evalCfg != nullptr)
                phase3Init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
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
            if (evalCfg != nullptr)
                init.addTestObject("preexisting_tableconfigs"_cs, tblName, evalCfg);
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
    size_t &outRepeat) {
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
                if (t.op != CpTerm::Op::Eq || t.actionDataArg.isNullOrEmpty()) continue;
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

    // H2S2C: condition sinks use a dedicated flow (symbolic Phase-3 flip confirmation for both
    // directions). Delegate before the table-specific logic below.
    if (currentSinkCondition != nullptr) {
        return runConditionChain(chain, initState, phase1Bucket, callBack, maxPerChain, missToHit);
    }
    if (missToHit && currentSinkTable_ == nullptr) return 0;

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

        // Hash/sketch/bloom-indexed SO register (tainted access index): pin Phase 2 to the exact
        // Phase-1 flow so the attacker collides with the victim's bucket (equal hash inputs ⇒ equal
        // bucket), instead of the distinctness NEQ that would move it to a different bucket.
        // Classify the SO register's index (see plan / runConditionChain for the rationale):
        // packet-derived index → pin only the index inputs to Phase 1 (keep port NEQ, drop table-key
        // NEQ); tainted RANDOM-hash index → whole-packet pin; constant index → full distinctness.
        const TestObject *soReg =
            (repPhase1State != nullptr)
                ? repPhase1State->getExecutionState()->getTestObject("registervalues"_cs,
                                                                     chain.soName, /*checked=*/false)
                : nullptr;
        const auto indexSymVars = collectIndexSymVars(soReg);
        if (indexSymVars.empty() && soReg != nullptr && soReg->hasTaintedIndex()) {
            pinPacketToPhase1(phase2Init, repPhase1State, cond1.inputPort, inputPortSymExpr);
        } else {
            // Constrain Phase 2's input port to differ from Phase 1's input AND output. A dropped
            // Phase-1 baseline has no output port (cond1.outputPort < 0), so only the input NEQ
            // applies.
            phase2Init.pushPathConstraint(new IR::Neq(
                inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
            if (cond1.outputPort >= 0)
                phase2Init.pushPathConstraint(new IR::Neq(
                    inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
            if (!indexSymVars.empty()) {
                pinIndexInputsToPhase1(phase2Init, repPhase1State, indexSymVars);
            } else {
                // Constrain Phase 2's table match keys to differ from Phase 1's (compatible,
                // coexisting entries). Skip size-1 tables: their entry is pre-injected.
                const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
                for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
                    if (size1Set.count(tblName) > 0) continue;
                    for (const auto &[keyName, match] : keyMap) {
                        phase2Init.pushPathConstraint(
                            match->buildTableKeyNeqConstraint(tblName, keyName));
                    }
                }
            }
        }
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
                        if (t.op == CpTerm::Op::Eq && !t.actionDataArg.isNullOrEmpty()) {
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
            const FinalState *fs3Drive = driveRegisterPhase2(
                chain, initState, driveTemplate, repPhase1State, cond1.inputPort, inputPortSymExpr,
                /*p3Target=*/missToHit ? 1 : 0, fs2real, kDrive);
            // Adopt the accumulation result when the write path was unreachable in one packet, or
            // when it genuinely needs k>1 (the single-packet terminals cannot flip this threshold).
            if (fs3Drive != nullptr && (singleWriteEmpty || kDrive > 1)) {
                if (!singleWriteEmpty) phase2StateMap[i].clear();
                phase2StateMap[i].push_back(fs2real);
                drivenFs3[fs2real] = fs3Drive;
                drivenRepeat[fs2real] = kDrive;
                drivenFs1[fs2real] = repPhase1State;
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
                for (const auto &[regName, regObj] :
                     fs2->getExecutionState()->getTestObjectCategory("registervalues"_cs)) {
                    if (regName != chain.soName) continue;
                    auto attackerResult = regObj->withAttackerValues(
                        fs2->getFinalModel(), SymbexOptions::get().stateTamperValue, forbiddenValues);
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
                    // fs3's SO-register read (its index conditions carry the flipped value at the
                    // pinned index), re-deriving concrete Phase-1/Phase-2 packets so their CRC hashes
                    // resolve eagerly and processPhase re-solves SAT — exactly as MISS→HIT does.
                    const auto &model3 = fs3->getFinalModel();
                    const auto *fs3SoReg = fs3->getExecutionState()->getTestObject(
                        "registervalues"_cs, chain.soName, false);
                    if (fs3SoReg == nullptr) continue;
                    std::map<cstring, const TestObject *> accRegValues;
                    accRegValues[chain.soName] =
                        fs3SoReg->withAttackerValues(model3, SymbexOptions::get().stateTamperValue, {})
                            .testObject;
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
                    TamperingFinalState tsAcc{*fs1c, *fs2c, false, cond1.inputPort, cond1.outputPort,
                                              ipAcc, opAcc, accRegValues, {}, accRegSinkTables, {}};
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
                    callBack(tsAcc);
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
                callBack(ts);
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

            // Build the emitted attacker register from Phase 3's read of the SO register: its index
            // conditions hold the (pinned, == Phase-1) read index, and withAttackerValues stamps the
            // tampered value there so the emitter produces an affected_register the harness can
            // pre-set. The carry above (evaluateForCarry) drove the symbolic run but has no index
            // conditions, so it cannot be emitted directly.
            const auto &model3 = fs3->getFinalModel();
            std::map<cstring, const TestObject *> attackerRegValues;
            const auto *fs3SoReg =
                fs3->getExecutionState()->getTestObject("registervalues"_cs, chain.soName, false);
            if (fs3SoReg == nullptr) continue;
            // Empty forbidden set: feasibility is always true here; the value becomes the real
            // (carried) Phase-2 write, and the symbolic Phase-3 confirmation (evalSinkHit==HIT
            // above) is the correctness gate. Overrides are unused for MISS→HIT (carry handles it).
            const auto *attackerReg = fs3SoReg
                                          ->withAttackerValues(model3,
                                                               SymbexOptions::get().stateTamperValue,
                                                               {})
                                          .testObject;
            attackerRegValues[chain.soName] = attackerReg;

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
            TamperingFinalState ts{*fs1c, *fs2c, false,
                                   cond1.inputPort, cond1.outputPort, ip2, op2,
                                   attackerRegValues, {}, attackerRegSinkTables, {}};
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.phase2RepeatCount = repeat;
            ts.missToHit = true;
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
            callBack(ts);
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
        // Hash/sketch/bloom-indexed SO register: its access index is tainted, so symbex can't tell
        // which bucket a packet maps to. The attacker packet must COLLIDE with the Phase-1 flow's
        // bucket, so pin Phase-2 to the exact Phase-1 packet (equal hash inputs ⇒ equal bucket)
        // instead of forcing it to differ — the distinctness NEQ would land it in a different bucket
        // (see ACC-Turbo: dst_addr-NEQ moved the attacker off the victim's bloom slot).
        // Classify the SO register's index and constrain Phase 2 accordingly (see plan):
        //   - packet-derived index (e.g. a concolic CRC hash): pin ONLY the index-determining inputs
        //     to Phase 1 so the attacker hits the victim's bucket; keep the port NEQ and DROP the
        //     table-key NEQ (it would conflict with a pinned hash-operand key). Other fields stay free.
        //   - tainted index (RANDOM hash, operands unrecoverable): pin the whole packet (same flow/path).
        //   - constant index: full distinctness (port + table-key NEQ), unchanged.
        const TestObject *soReg =
            (repPhase1State != nullptr)
                ? repPhase1State->getExecutionState()->getTestObject("registervalues"_cs,
                                                                     chain.soName, /*checked=*/false)
                : nullptr;
        const auto indexSymVars = collectIndexSymVars(soReg);
        if (indexSymVars.empty() && soReg != nullptr && soReg->hasTaintedIndex()) {
            pinPacketToPhase1(phase2Init, repPhase1State, cond1.inputPort, inputPortSymExpr);
        } else {
            phase2Init.pushPathConstraint(new IR::Neq(
                inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
            if (cond1.outputPort >= 0)
                phase2Init.pushPathConstraint(new IR::Neq(
                    inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
            if (!indexSymVars.empty()) {
                pinIndexInputsToPhase1(phase2Init, repPhase1State, indexSymVars);
            } else {
                const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
                for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
                    if (size1Set.count(tblName) > 0) continue;
                    for (const auto &[keyName, match] : keyMap)
                        phase2Init.pushPathConstraint(
                            match->buildTableKeyNeqConstraint(tblName, keyName));
                }
            }
        }
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
            const FinalState *fs3Drive =
                driveRegisterPhase2(chain, initState, driveTemplate, repPhase1State, cond1.inputPort,
                                    inputPortSymExpr, p3Target, fs2real, kDrive);
            // Adopt the accumulation result when the write path was unreachable in one packet, or when
            // it genuinely needs k>1 (the single-packet terminals cannot flip this threshold).
            if (fs3Drive != nullptr && (singleWriteEmpty || kDrive > 1)) {
                if (!singleWriteEmpty) phase2StateMap[i].clear();
                phase2StateMap[i].push_back(fs2real);
                drivenFs3[fs2real] = fs3Drive;
                drivenRepeat[fs2real] = kDrive;
                drivenFs1[fs2real] = repPhase1State;
            }
        }
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

            const auto &model3 = fs3->getFinalModel();
            std::map<cstring, const TestObject *> attackerRegValues;
            const auto *fs3SoReg =
                fs3->getExecutionState()->getTestObject("registervalues"_cs, chain.soName, false);
            if (fs3SoReg == nullptr) continue;
            const auto *attackerReg =
                fs3SoReg->withAttackerValues(model3, SymbexOptions::get().stateTamperValue, {})
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
            TamperingFinalState ts{*fs1, *fs2, false, cond1.inputPort, p1OutPort, ip2, p2OutPort,
                                   attackerRegValues, {}, {}, {}};
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.phase2RepeatCount = repeat;
            ts.missToHit = missToHit;
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
