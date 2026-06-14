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
        case StateDependencyPolicy::AlteringPath:
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
        case StateDependencyPolicy::AlteringPath:
            for (const auto &[v, node] : chain.writeNodes)
                if (node != nullptr) nodes.insert(node);
            for (const auto &[v, node] : chain.readNodes)
                if (node != nullptr) nodes.insert(node);
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
    cstring sinkTableName_ = ""_cs;
    std::vector<cstring> extraSkippedTables_;
    ScopedSymbexOpts(bool setOutputPacketOnly, cstring sinkTableName = ""_cs,
                     std::vector<cstring> extraSkippedTables = {},
                     bool setRegTracking = true,
                     bool isPhase1 = false)
        : extraSkippedTables_(std::move(extraSkippedTables)) {
        auto &opts = SymbexOptions::get();
        savedOutputPacketOnly             = opts.outputPacketOnly;
        savedCoverStatements              = opts.coverageOptions.coverStatements;
        savedTamperingRegisterTracking    = opts.tamperingRegisterTracking;
        savedInitRegZeroValue        = opts.initRegZeroValue;
        opts.outputPacketOnly             = setOutputPacketOnly;
        opts.coverageOptions.coverStatements = true;
        opts.tamperingRegisterTracking    = setRegTracking;
        opts.initRegZeroValue        = isPhase1;
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

    auto chains = collectChains();

    // Flatten all chains for the shared Phase-1 pass.
    allChains.clear();
    for (const auto &[chainName, chainList] : chains)
        for (const auto *chain : chainList) allChains.push_back(chain);
    if (allChains.empty()) return;

    // One shared Phase-1 traversal of the whole program collects read-baseline terminals for EVERY
    // chain at once (bucketed per chain), instead of re-traversing the program once per chain.
    collectPhase1Terminals(initState);

    // Per-chain cap on emitted sub-tests, reusing the existing --max-tests option. Applied per chain
    // (not globally) so every SOChain produces its own tests. 0 means "unlimited".
    const size_t maxPerChain = static_cast<size_t>(SymbexOptions::get().maxTests);
    for (const auto &[chainName, chainList] : chains) {
        for (const auto *chain : chainList) {
            currentChain = chain;
            currentChainName = chainName;
            printInfo("============ Chain (%1%) id=%2% %3% [Tampering 3-phase] ============",
                      chainName, chain->id, chain->soName);
            const auto &bucket = phase1Buckets[chain->id];
            runTamperingChain(*chain, initState, bucket, callBack, maxPerChain, /*missToHit=*/false);
            runTamperingChain(*chain, initState, bucket, callBack, maxPerChain, /*missToHit=*/true);
        }
    }
}

// ---------------------------------------------------------------------------
// Shared Phase-1 collection: one traversal, terminals bucketed per chain
// ---------------------------------------------------------------------------

bool StateDependencyTracker::chainTargetsCovered(
    const P4StateDependency::DependencyGraphs::SOChain &chain,
    const P4::Coverage::CoverageSet &visited) const {
    auto it = chainPhase1Targets.find(chain.id);
    if (it == chainPhase1Targets.end() || it->second.empty()) return false;
    return std::all_of(it->second.begin(), it->second.end(),
                       [&visited, &chain, this](const IR::Node *n) {
                           if (n->is<IR::Key>())
                               return isTableVisited(chain.sinkTableControlPlaneName, visited);
                           return visited.count(n) > 0;
                       });
}

bool StateDependencyTracker::conditionReached(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &es) const {
    if (chain.sinkConditionNode == nullptr) return false;
    const auto *ifStmt = chain.sinkConditionNode->to<IR::IfStatement>();
    if (ifStmt == nullptr) return false;
    // The branch-stamped condition var is set iff the if-statement was reached on this path.
    return es.get(CmdStepper::getConditionVar(ifStmt)) != nullptr;
}

bool StateDependencyTracker::allPhase1BucketsFull() const {
    for (const auto *ch : allChains) {
        auto it = phase1Buckets.find(ch->id);
        if (it == phase1Buckets.end() || it->second.size() < phase1BucketCap) return false;
    }
    return true;
}

void StateDependencyTracker::handleSharedTerminal(const ExecutionState &es) {
    ++phase1Examined;
    const auto &visited = es.getVisited();
    std::vector<size_t> matched;
    for (const auto *ch : allChains) {
        if (phase1Buckets[ch->id].size() >= phase1BucketCap) continue;  // bucket already full
        // Condition chains (H2S2C): bucket iff the if-condition was reached (baseline value exists).
        // Key chains: require read-node coverage.
        const bool covered = (ch->sinkConditionNode != nullptr) ? conditionReached(*ch, es)
                                                                : chainTargetsCovered(*ch, visited);
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
    const auto *condExpr = fs->getExecutionState()->get(condVar);
    if (condExpr == nullptr) return -1;  // condition not reached
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

const FinalState *StateDependencyTracker::runSymbolicPhase3(
    const P4StateDependency::DependencyGraphs::SOChain &chain, const ExecutionState &initState,
    const FinalState *fs1, int inputPort, const IR::Expression *inputPortSymExpr,
    const std::map<cstring, const TestObject *> &carriedRegs) {
    const auto &model1 = fs1->getFinalModel();
    const auto *p1PktExpr = fs1->getExecutionState()->getInputPacket();
    const auto *p1PktSize = model1.evaluate(ExecutionState::getInputPacketSizeVar(), true);

    currentPhase = TamperingPhase::Phase3_Read;
    if (currentSinkCondition != nullptr) {
        // The pinned-input replay is (almost) deterministic, so accept ANY terminal and check the
        // condition flip externally via evalCondition. Requiring writeNode coverage would reject
        // every terminal for update chains (their writeNodes span mutually-exclusive branches), and
        // reaching-set pruning would cut the path before a terminal. So: no required nodes, no prune.
        currentRequiredNodes.clear();
        reachingSet_.clear();
        reachingSetValid_ = false;
    } else {
        currentRequiredNodes = buildRequiredNodes(chain);
        buildReachingSet();
    }

    auto &phase3Init = initState.clone();
    // Pre-set the (tampered) register state the caller computed.
    for (const auto &[regName, regObj] : carriedRegs)
        phase3Init.addTestObject("registervalues"_cs, regName, regObj);

    // Replay Phase 1's exact input. The accumulated input-packet expression is a Concat rooted at a
    // zero-width constant (the initial empty packet), which Z3 cannot translate; so instead of
    // constraining the whole expression we pin each pktvar_N *symbolic variable* it contains to its
    // Phase-1 value. Cloning initState re-pulls the same pktvar_N in order, so this replays
    // Phase-1's bytes (same register index + header-derived key fields).
    std::function<void(const IR::Expression *)> pinPktVars = [&](const IR::Expression *e) {
        if (e == nullptr) return;
        if (const auto *sv = e->to<IR::SymbolicVariable>()) {
            phase3Init.pushPathConstraint(new IR::Equ(sv, model1.evaluate(sv, true)));
        } else if (const auto *cc = e->to<IR::Concat>()) {
            pinPktVars(cc->left);
            pinPktVars(cc->right);
        } else if (const auto *sl = e->to<IR::Slice>()) {
            pinPktVars(sl->e0);
        }
    };
    pinPktVars(p1PktExpr);
    phase3Init.pushPathConstraint(new IR::Equ(ExecutionState::getInputPacketSizeVar(), p1PktSize));
    phase3Init.pushPathConstraint(
        new IR::Equ(inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, inputPort)));

    std::vector<const FinalState *> phase3States;
    {
        // Carry registers (isPhase1=false ⇒ no zero-init); the sink uses its own entries.
        ScopedSymbexOpts guard(/*outputPacketOnly=*/false, ""_cs, {}, /*setRegTracking=*/true,
                               /*isPhase1=*/false);
        runPhase(phase3Init, phase3States, 1);
    }
    return phase3States.empty() ? nullptr : phase3States[0];
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

        // Constrain Phase 2's input port to differ from Phase 1's input AND output. A dropped
        // Phase-1 baseline has no output port (cond1.outputPort < 0), so only the input NEQ applies.
        phase2Init.pushPathConstraint(new IR::Neq(
            inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
        if (cond1.outputPort >= 0)
            phase2Init.pushPathConstraint(new IR::Neq(
                inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
        // Constrain Phase 2's table match keys to differ from Phase 1's (compatible, coexisting
        // entries). Skip size-1 tables: their entry is pre-injected.
        const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
        for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
            if (size1Set.count(tblName) > 0) continue;
            for (const auto &[keyName, match] : keyMap) {
                phase2Init.pushPathConstraint(match->buildTableKeyNeqConstraint(tblName, keyName));
            }
        }
        runPhase(phase2Init, phase2StateMap[i], maxPerChain);
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
            }
        }

        // Round-robin emission across Phase-1 states so the per-chain cap never starves a later
        // Phase-1 read-state: every Phase-1 state contributes one sub-test before any gets a second.
        size_t subTestId = 0;
        std::vector<size_t> cursor(phase1States.size(), 0);
        bool chainCapHit = false;
        while (!chainCapHit) {
            bool emittedThisRound = false;
            for (size_t i = 0; i < phase1States.size() && !chainCapHit; ++i) {
                auto &fs2List = phase2StateMap.at(phase1StateToCondition.at(i));
                if (cursor[i] >= fs2List.size()) continue;  // Phase-1 state exhausted
                const auto *fs1 = phase1States[i];
                const auto &cond1 = phase1Conditions[phase1StateToCondition.at(i)];
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
                if (!soFeasible) {
                    printInfo("[Tampering] Phase 2 packet for chain id=%1%: constant register write "
                              "does not flip sink '%2%'; trying another Phase-2 packet.",
                              chain.id, chain.soName);
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
                                // control-plane key (conflicting entries).
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
                                    p1ExtraConstraints.push_back(
                                        match2->buildPacketFieldNeqConstraint(pktField));
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

        for (const auto *fs2 : phase2StateMap[phase1StateToCondition[i]]) {
            if (emitted >= maxPerChain) break;

            // Carry the tampered (post-Phase-2) register contents into Phase 3.
            const auto &model2 = fs2->getFinalModel();
            const auto *es2 = fs2->getExecutionState();
            std::map<cstring, const TestObject *> carriedRegs;
            bool carriedSo = false;
            for (const auto &[regName, regObj] :
                 es2->getTestObjectCategory("registervalues"_cs)) {
                carriedRegs[regName] = regObj->evaluateForCarry(model2);
                if (regName == chain.soName) carriedSo = true;
            }
            if (!carriedSo) continue;
            const auto *fs3 = runSymbolicPhase3(chain, initState, fs1, cond1.inputPort,
                                                inputPortSymExpr, carriedRegs);
            if (fs3 == nullptr) continue;       // pinned input had no terminal (unsatisfiable)
            if (evalSinkHit(fs3) != 1) continue;  // sink did not flip MISS→HIT
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

            int ip2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetInputPortVar()), true));
            int op2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetOutputPortVar()), true));
            std::map<cstring, cstring> attackerRegSinkTables;
            attackerRegSinkTables[chain.soName] = chain.sinkTableControlPlaneName;

            // Emit a sink-flip test: Phase 1 (its own disposition is the validator's reference) →
            // Phase 3 (replay with the tampered register). The flip MISS→HIT is symbolically
            // confirmed above; the end-to-end validator observes the actual Phase-1→Phase-3 output
            // divergence (drop/port/bytes), so p4symbex does not emit a predicted phase3_verify.
            TamperingFinalState ts{*fs1, *fs2, false,
                                   cond1.inputPort, cond1.outputPort, ip2, op2,
                                   attackerRegValues, {}, attackerRegSinkTables, {}};
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.missToHit = true;
            ts.caseLabel = cstring("MISS_TO_HIT/" + disp);
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
        phase2Init.pushPathConstraint(new IR::Neq(
            inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.inputPort)));
        if (cond1.outputPort >= 0)
            phase2Init.pushPathConstraint(new IR::Neq(
                inputPortSymExpr, IR::Constant::get(inputPortSymExpr->type, cond1.outputPort)));
        const std::set<cstring> size1Set(size1Tables.begin(), size1Tables.end());
        for (const auto &[tblName, keyMap] : cond1.tableKeyMap) {
            if (size1Set.count(tblName) > 0) continue;
            for (const auto &[keyName, match] : keyMap)
                phase2Init.pushPathConstraint(match->buildTableKeyNeqConstraint(tblName, keyName));
        }
        runPhase(phase2Init, phase2StateMap[i], maxPerChain);
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
        for (const auto *fs2 : phase2StateMap[phase1StateToCondition[i]]) {
            if (emitted >= maxPerChain) break;
            const auto &model2 = fs2->getFinalModel();
            const auto *es2 = fs2->getExecutionState();
            std::map<cstring, const TestObject *> carriedRegs;
            bool carriedSo = false;
            for (const auto &[regName, regObj] :
                 es2->getTestObjectCategory("registervalues"_cs)) {
                carriedRegs[regName] = regObj->evaluateForCarry(model2);
                if (regName == chain.soName) carriedSo = true;
            }
            if (!carriedSo) continue;
            const auto *fs3 = runSymbolicPhase3(chain, initState, fs1, cond1.inputPort,
                                                inputPortSymExpr, carriedRegs);
            if (fs3 == nullptr) continue;                  // pinned input unsatisfiable
            if (evalCondition(fs3) != p3Target) continue;  // condition did not flip
            if (!sinkConditionDiverges()) continue;        // then/else write the same output

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
            int ip2 = IR::getIntFromLiteral(
                model2.evaluate(es2->get(programInfo.getTargetInputPortVar()), true));
            // Condition sink: no table, so attackerRegisterSinkTables stays empty.
            TamperingFinalState ts{*fs1, *fs2, false, cond1.inputPort, p1OutPort, ip2, p2OutPort,
                                   attackerRegValues, {}, {}, {}};
            ts.chainId = chain.id;
            ts.subTestId = ++emitted;
            ts.missToHit = missToHit;
            ts.caseLabel = cstring(missToHit ? "COND_FALSE_TO_TRUE" : "COND_TRUE_TO_FALSE");
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
                } else {
                    // Only emit a test when the path covers all required nodes.
                    const auto &visited = executionState.get().getVisited();
                    bool allCovered =
                        std::all_of(currentRequiredNodes.begin(), currentRequiredNodes.end(),
                                    [&visited, this](const IR::Node *n) {
                                        // IR::Key nodes are never passed to markVisited; instead
                                        // check whether the table that owns this key had its
                                        // apply() MethodCallStatement visited.
                                        if (n->is<IR::Key>()) {
                                            return isTableVisited(
                                                currentChain->sinkTableControlPlaneName, visited);
                                        }
                                        return visited.count(n) > 0;
                                    });
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
                            bool hit = n->is<IR::Key>()
                                ? isTableVisited(currentChain->sinkTableControlPlaneName, visited)
                                : visited.count(n) > 0;
                            printInfo("  %1% [%2%] %3% %4%", (hit ? "OK  " : "MISS"),
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
