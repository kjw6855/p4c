#include "backends/p4tools/modules/symbex/core/small_step/table_stepper.h"

#include <algorithm>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/number.hpp>

#include "backends/p4tools/common/control_plane/symbolic_variables.h"
#include "backends/p4tools/common/lib/constants.h"
#include "backends/p4tools/common/lib/logging.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/common/lib/trace_event_types.h"
#include "backends/p4tools/common/lib/variables.h"
#include "ir/id.h"
#include "ir/indexed_vector.h"
#include "ir/irutils.h"
#include "ir/vector.h"
#include "lib/error.h"
#include "lib/exceptions.h"
#include "lib/log.h"
#include "lib/null.h"
#include "lib/source_file.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/symbex/core/program_info.h"
#include "backends/p4tools/modules/symbex/core/small_step/expr_stepper.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_annotation.h"
#include "backends/p4tools/modules/symbex/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/symbex/lib/collect_coverable_nodes.h"
#include "backends/p4tools/modules/symbex/lib/continuation.h"
#include "backends/p4tools/modules/symbex/lib/exceptions.h"
#include "backends/p4tools/modules/symbex/lib/execution_state.h"
#include "backends/p4tools/modules/symbex/lib/test_spec.h"
#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

const ExecutionState *TableStepper::getExecutionState() { return &stepper->state; }

const ProgramInfo *TableStepper::getProgramInfo() { return &stepper->programInfo; }

ExprStepper::Result TableStepper::getResult() { return stepper->result; }

const IR::StateVariable &TableStepper::getTableStateVariable(const IR::Type *type,
                                                             const IR::P4Table *table, cstring name,
                                                             std::optional<int> idx1_opt,
                                                             std::optional<int> idx2_opt) {
    // Mash the table name, the given name, and the optional indices together.
    // XXX To be nice, we should probably build a PathExpression, but that's annoying to do, and we
    // XXX can probably get away with this.
    std::stringstream out;
    out << table->name.toString() << "." << name;
    if (idx1_opt.has_value()) {
        out << "." << idx1_opt.value();
    }
    if (idx2_opt.has_value()) {
        out << "." << idx2_opt.value();
    }

    return ToolsVariables::getStateVariable(type, out.str());
}

const IR::StateVariable &TableStepper::getTableActionVar(const IR::P4Table *table) {
    auto numActions = table->getActionList()->size();
    size_t max = 255;
    BUG_CHECK(numActions < max, "Number of actions in the table (%1%) exceeds the maximum of %2%.",
              numActions, max);
    return getTableStateVariable(IR::Type_Bits::get(8), table, "*action"_cs);
}

const IR::StateVariable &TableStepper::getTableResultVar(const IR::P4Table *table) {
    return getTableStateVariable(IR::Type::Boolean::get(), table, "*result"_cs);
}

const IR::StateVariable &TableStepper::getActiveTableVar() {
    return ToolsVariables::getStateVariable(IR::Type::String::get(), "*active_table_var"_cs);
}

const IR::StateVariable &TableStepper::getTableHitVar(const IR::P4Table *table) {
    return getTableStateVariable(IR::Type::Boolean::get(), table, "*hit"_cs);
}

const IR::Expression *TableStepper::computeTargetMatchType(
    const TableUtils::KeyProperties &keyProperties, TableMatchMap *matches,
    const IR::Expression *hitCondition) {
    const IR::Expression *keyExpr = keyProperties.key->expression;
    // Create a new variable constant that corresponds to the key expression.
    const auto *ctrlPlaneKey =
        ControlPlaneState::getTableKey(properties.tableName, keyProperties.name, keyExpr->type);

    if (keyProperties.matchType == P4Constants::MATCH_KIND_EXACT) {
        hitCondition = new IR::LAnd(hitCondition, new IR::Equ(keyExpr, ctrlPlaneKey));
        matches->emplace(keyProperties.name, new Exact(keyProperties.key, ctrlPlaneKey));
        return hitCondition;
    }
    if (keyProperties.matchType == P4Constants::MATCH_KIND_TERNARY) {
        const IR::Expression *ternaryMask = nullptr;
        // We can recover from taint by inserting a ternary match that is 0.
        if (keyProperties.isTainted) {
            ternaryMask = IR::Constant::get(keyExpr->type, 0);
            keyExpr = ternaryMask;
        } else {
            ternaryMask = ControlPlaneState::getTableTernaryMask(properties.tableName,
                                                                 keyProperties.name, keyExpr->type);
        }
        matches->emplace(keyProperties.name,
                         new Ternary(keyProperties.key, ctrlPlaneKey, ternaryMask));
        return new IR::LAnd(hitCondition, new IR::Equ(new IR::BAnd(keyExpr, ternaryMask),
                                                      new IR::BAnd(ctrlPlaneKey, ternaryMask)));
    }
    if (keyProperties.matchType == P4Constants::MATCH_KIND_LPM) {
        const auto *keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
        auto keyWidth = keyType->width_bits();
        const IR::Expression *maskVar = ControlPlaneState::getTableMatchLpmPrefix(
            properties.tableName, keyProperties.name, keyExpr->type);
        // The maxReturn is the maximum vale for the given bit width. This value is shifted by
        // the mask variable to create a mask (and with that, a prefix).
        auto maxReturn = IR::getMaxBvVal(keyWidth);
        auto *prefix = new IR::Sub(IR::Constant::get(keyType, keyWidth), maskVar);
        const IR::Expression *lpmMask = nullptr;
        // We can recover from taint by inserting a ternary match that is 0.
        if (keyProperties.isTainted) {
            lpmMask = IR::Constant::get(keyExpr->type, 0);
            maskVar = lpmMask;
            keyExpr = lpmMask;
        } else {
            lpmMask = new IR::Shl(IR::Constant::get(keyType, maxReturn), prefix);
        }
        matches->emplace(keyProperties.name, new LPM(keyProperties.key, ctrlPlaneKey, maskVar));
        return new IR::LAnd(
            hitCondition,
            new IR::LAnd(
                // This is the actual LPM match under the shifted mask (the prefix).
                new IR::Leq(maskVar, IR::Constant::get(keyType, keyWidth)),
                // The mask variable shift should not be larger than the key width.
                new IR::Equ(new IR::BAnd(keyExpr, lpmMask), new IR::BAnd(ctrlPlaneKey, lpmMask))));
    }

    SYMBEX_UNIMPLEMENTED("Match type %s not implemented for table keys.", keyProperties.matchType);
}

const IR::Expression *TableStepper::computeHit(TableMatchMap *matches) {
    const IR::Expression *hitCondition = IR::BoolLiteral::get(!properties.resolvedKeys.empty());
    for (auto keyProperties : properties.resolvedKeys) {
        hitCondition = computeTargetMatchType(keyProperties, matches, hitCondition);
    }
    return hitCondition;
}

const IR::StringLiteral *TableStepper::getTableActionString(
    const IR::MethodCallExpression *actionCall) {
    return IR::StringLiteral::get(actionCall->method->toString());
}

const IR::Expression *TableStepper::evalTableConstEntries() {
    const IR::Expression *tableMissCondition = IR::BoolLiteral::get(true);

    const auto *key = table->getKey();
    BUG_CHECK(key != nullptr, "An empty key list should have been handled earlier.");

    const auto *entries = table->getEntries();
    // Sometimes, there are no entries.
    if (entries == nullptr) {
        // If a pre-existing TableConfig was injected into the state (e.g. Phase 1's entry for
        // a size-1 table during Phase 2 evaluation), evaluate it as HIT/MISS branches instead
        // of returning MISS-only.  Uses "preexisting_tableconfigs" so the entry is not picked
        // up by processPhase (which reads only "tableconfigs") and thus not duplicated in output.
        const auto *preExisting = stepper->state.getTestObject(
            "preexisting_tableconfigs"_cs, properties.tableName, /*checked=*/false);
        if (preExisting != nullptr) {
            const auto *cfg = preExisting->to<TableConfig>();
            if (cfg != nullptr && !cfg->getRules()->empty())
                return evalTablePreExistingConfig(*cfg);
        }
        return tableMissCondition;
    }

    auto entryVector = entries->entries;

    // Sort entries if one of the key contains an LPM match.
    for (size_t idx = 0; idx < key->keyElements.size(); ++idx) {
        const auto *keyElement = key->keyElements.at(idx);
        if (keyElement->matchType->path->toString() == P4Constants::MATCH_KIND_LPM) {
            std::sort(entryVector.begin(), entryVector.end(), [idx](auto &&PH1, auto &&PH2) {
                return TableUtils::compareLPMEntries(std::forward<decltype(PH1)>(PH1),
                                                     std::forward<decltype(PH2)>(PH2), idx);
            });
            break;
        }
    }

    for (const auto *entry : entryVector) {
        const auto *action = entry->getAction();
        const auto *tableAction = action->checkedTo<IR::MethodCallExpression>();
        const auto *actionType = stepper->state.getP4Action(tableAction);
        auto &nextState = stepper->state.clone();
        nextState.markVisited(entry);
        // Compute the table key for a constant entry
        const auto *hitCondition = TableUtils::computeEntryMatch(*table, *entry, *key);

        // Update all the tracking variables for tables.
        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(new IR::MethodCallStatement(Util::SourceInfo(), tableAction));
        nextState.set(getTableHitVar(table), IR::BoolLiteral::get(true));
        nextState.set(getTableActionVar(table), getTableActionString(tableAction));
        nextState.set(getActiveTableVar(), IR::StringLiteral::get(table->name));

        // Some path selection strategies depend on looking ahead and collecting potential
        // nodes. If that is the case, apply the CoverableNodesScanner visitor.
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(SymbexOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(stepper->state);
            collector.updateNodeCoverage(actionType, coveredNodes);
        }

        // Add some tracing information.
        std::stringstream tableStream;
        tableStream << "Constant Table Branch: " << properties.tableName;
        bool isFirstKey = true;
        const auto &keyElements = key->keyElements;

        for (const auto *keyElement : keyElements) {
            if (isFirstKey) {
                tableStream << " | Key(s): ";
            } else {
                tableStream << ", ";
            }
            tableStream << keyElement->expression;
            isFirstKey = false;
        }
        tableStream << " | Chosen action: " << tableAction->toString();
        const auto *args = tableAction->arguments;
        bool isFirstArg = true;
        for (const auto *arg : *args) {
            if (isFirstArg) {
                tableStream << " | Arg(s): ";
            } else {
                tableStream << ", ";
            }
            tableStream << arg->expression;
            isFirstArg = false;
        }
        nextState.add(*new TraceEvents::Generic(tableStream.str()));
        nextState.replaceTopBody(&replacements);
        // Update the default condition.
        // The default condition can only be triggered, if we do not hit this match.
        // We encode this constraint in this expression.
        stepper->result->emplace_back(new IR::LAnd(tableMissCondition, hitCondition),
                                      stepper->state, nextState, coveredNodes);
        tableMissCondition = new IR::LAnd(new IR::LNot(hitCondition), tableMissCondition);
    }
    return tableMissCondition;
}

const IR::Expression *TableStepper::evalTablePreExistingConfig(const TableConfig &cfg) {
    const IR::Expression *tableMissCondition = IR::BoolLiteral::get(true);
    const auto *key = table->getKey();
    BUG_CHECK(key != nullptr, "An empty key list should have been handled earlier.");

    for (const auto &rule : *cfg.getRules()) {
        const IR::Expression *hitCondition = IR::BoolLiteral::get(true);

        // Build hit condition from the rule's concrete match values.
        for (const auto &keyProp : properties.resolvedKeys) {
            auto matchIt = rule.getMatches()->find(keyProp.name);
            if (matchIt == rule.getMatches()->end()) continue;
            const auto *matchObj = matchIt->second;
            const IR::Expression *keyExpr = keyProp.key->expression;

            if (const auto *exact = matchObj->to<Exact>()) {
                hitCondition = new IR::LAnd(
                    hitCondition, new IR::Equ(keyExpr, exact->getEvaluatedValue()));
            } else if (const auto *ternary = matchObj->to<Ternary>()) {
                const auto *mask = ternary->getEvaluatedMask();
                hitCondition = new IR::LAnd(
                    hitCondition,
                    new IR::Equ(new IR::BAnd(keyExpr, mask),
                                new IR::BAnd(ternary->getEvaluatedValue(), mask)));
            } else if (const auto *lpm = matchObj->to<LPM>()) {
                const auto *prefix = lpm->getEvaluatedPrefixLength();
                const auto *keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
                auto width = keyType->width_bits();
                auto maxVal = IR::getMaxBvVal(width);
                const IR::Expression *shift =
                    new IR::Sub(IR::Constant::get(keyType, width), prefix);
                const IR::Expression *lpmMask =
                    new IR::Shl(IR::Constant::get(keyType, maxVal), shift);
                hitCondition = new IR::LAnd(
                    hitCondition,
                    new IR::Equ(new IR::BAnd(keyExpr, lpmMask),
                                new IR::BAnd(lpm->getEvaluatedValue(), lpmMask)));
            }
        }

        // Find the matching action in the table's declared action list.
        const auto *ruleAction = rule.getActionCall();
        const IR::MethodCallExpression *tableAction = nullptr;
        const IR::P4Action *actionType = nullptr;
        for (const auto *actionElem : TableUtils::buildTableActionList(*table)) {
            const auto *mce = actionElem->expression->checkedTo<IR::MethodCallExpression>();
            const auto *act = stepper->state.getP4Action(mce);
            if (act->controlPlaneName() == ruleAction->getActionName()) {
                tableAction = mce;
                actionType = act;
                break;
            }
        }
        if (tableAction == nullptr) {
            warning("[pre-existing entry] action '%1%' not found in table '%2%'; skipping HIT.",
                    ruleAction->getActionName(), properties.tableName);
            continue;
        }

        // Reconstruct the method call with the rule's concrete arguments.
        // getEvaluatedValue() converts BoolLiteral → Constant (bit<1>), which causes a type
        // mismatch when the parameter is bool (Type_Boolean). Restore the BoolLiteral in that case.
        auto *synthesizedAction = tableAction->clone();
        auto *arguments = new IR::Vector<IR::Argument>();
        for (const auto &arg : *ruleAction->getArgs()) {
            const IR::Expression *argExpr = arg.getEvaluatedValue();
            const auto *param = arg.getActionParam();
            if (param != nullptr && param->type->is<IR::Type_Boolean>()) {
                argExpr = IR::BoolLiteral::get(arg.getEvaluatedValue()->value != 0);
            }
            arguments->push_back(new IR::Argument(argExpr));
        }
        synthesizedAction->arguments = arguments;

        // Create the HIT branch state.
        auto &nextState = stepper->state.clone();
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(SymbexOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(stepper->state);
            collector.updateNodeCoverage(actionType, coveredNodes);
        }
        nextState.set(getTableHitVar(table), IR::BoolLiteral::get(true));
        nextState.set(getTableActionVar(table), getTableActionString(synthesizedAction));
        nextState.set(getActiveTableVar(), IR::StringLiteral::get(table->name));
        std::stringstream preExistingStream;
        preExistingStream << "Pre-existing Table Entry Hit: " << properties.tableName;
        nextState.add(*new TraceEvents::Generic(preExistingStream.str()));

        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(
            new IR::MethodCallStatement(Util::SourceInfo(), synthesizedAction));
        nextState.replaceTopBody(&replacements);

        stepper->result->emplace_back(new IR::LAnd(tableMissCondition, hitCondition),
                                      stepper->state, nextState, coveredNodes);
        tableMissCondition = new IR::LAnd(new IR::LNot(hitCondition), tableMissCondition);
    }
    return tableMissCondition;
}

namespace {

/// Trailing component of a dotted control-plane name ("SwitchIngress.tbl.act" -> "act").
std::string cpNameTail(cstring name) {
    const std::string s(name.string_view());
    auto pos = s.find_last_of('.');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

/// Control-plane names reach this file at different qualification levels: the IR carries the fully
/// qualified action name ("SwitchIngress.get_threshold.tbl_get_threshold_act") while annotations and
/// recorded ActionCalls may carry only the trailing component. Compare on both, the same convention
/// clausesFor uses for table names. (state_dependency_track.cpp has its own copy, cpNameMatches; it
/// is a file-static there and deliberately not exported.)
bool sameCpName(cstring a, cstring b) { return a == b || cpNameTail(a) == cpNameTail(b); }

/// Phase 1's default-action override for @p tableName, injected by the tampering executor as a
/// "pinned_default_actions" test object. A separate category from "preexisting_tableconfigs"
/// because that one is consumed only on the keyed path (evalTablePreExistingConfig BUG_CHECKs on a
/// null key), while a keyless table has no key at all and never reaches it.
///
/// The injected call carries ONLY arguments a --cp-annotation `action_data` clause fixed to a
/// literal (state_dependency_track.cpp's annotationBackedSubset): a value nobody constrained is
/// whatever the earlier phase's solver happened to pick, and forcing a later phase to it invents a
/// control-plane state that may not be deployable. Unconstrained data is left free here and the
/// tampering executor drops the candidate afterwards if the phases turn out to contradict.
const ActionCall *pinnedDefaultActionFor(const ExecutionState &state, cstring tableName) {
    const auto *obj = state.getTestObject("pinned_default_actions"_cs, tableName, /*checked=*/false);
    return obj == nullptr ? nullptr : obj->to<ActionCall>();
}

/// Equality tying the freshly synthesized action-argument symbol @p actionArg to the value the
/// pinned call recorded for the same parameter; nullptr when @p pinned does not cover @p parameter,
/// or when @p actionName is a different action than the one the pin was recorded for (each fork
/// mints its own symbols, so a same-named parameter of another action is a different variable).
const IR::Expression *pinnedDefaultArgPin(const ActionCall *pinned, cstring actionName,
                                          const IR::Parameter *parameter,
                                          const IR::Expression *actionArg) {
    if (pinned == nullptr || parameter == nullptr) return nullptr;
    if (!sameCpName(actionName, pinned->getActionName())) return nullptr;
    const auto *args = pinned->getArgs();
    if (args == nullptr) return nullptr;
    for (const auto &arg : *args) {
        const auto *param = arg.getActionParam();
        if (param == nullptr) continue;
        // The symbol below is built from (table, action, parameter->name, type) and ActionArg keeps
        // that very same IR::Parameter, so pointer identity is the normal case; the name compare
        // covers a re-resolved IR (e.g. a cache-loaded chain matched against a fresh program).
        if (param != parameter && param->name != parameter->name) continue;
        const auto *value = arg.getEvaluatedValue();
        if (value == nullptr) return nullptr;
        // getEvaluatedValue() turns a BoolLiteral into a bit<1> Constant, which does not type
        // against a bool parameter -- the same restoration evalTablePreExistingConfig does.
        if (parameter->type->is<IR::Type_Boolean>()) {
            return new IR::Equ(actionArg, IR::BoolLiteral::get(value->value != 0));
        }
        // Re-type the constant to the symbol's own type: the recorded value came out of a different
        // phase's model and only its numeric value is meaningful here.
        if (const auto *bits = actionArg->type->to<IR::Type_Bits>()) {
            return new IR::Equ(actionArg, IR::Constant::get(bits, value->value));
        }
        return new IR::Equ(actionArg, value);
    }
    return nullptr;
}

}  // namespace

const IR::Expression *TableStepper::cpActionArgPin(cstring actionName,
                                                   const IR::Parameter *parameter,
                                                   const IR::Expression *actionArg) const {
    const auto *ann = loadedCpAnnotation();
    if (ann == nullptr || parameter == nullptr) return nullptr;
    // Annotations name actions and parameters as the control plane sees them ("tbl_get_threshold_act",
    // "threshold"), while the IR carries fully qualified action names and midend-uniquified parameter
    // names ("SwitchIngress.get_threshold.tbl_get_threshold_act", "threshold_1"). controlPlaneName()
    // undoes the parameter renaming via the @name annotation; sameCpName handles the action
    // qualification, the same convention clausesFor uses for table names.
    const cstring paramName = parameter->controlPlaneName();
    for (const auto *c : ann->clausesFor(properties.tableName)) {
        for (const auto &t : c->when) {
            // hasValue, not `value != 0`: a term whose "value" is absent or unreadable pins
            // nothing. --dump-cp-stubs writes skeletons with "value": null, and CpTerm::value
            // defaults to 0, so without this an unfilled stub would silently pin the argument to 0
            // and every generated test would inherit a constraint nobody wrote.
            if (t.op != CpTerm::Op::Eq || t.actionDataArg.isNullOrEmpty() || !t.hasValue) continue;
            // An action_data term names the action it belongs to. A term that omits it applies to
            // whichever action carries a parameter of that name.
            if (!t.actionDataAction.isNullOrEmpty() && !sameCpName(actionName, t.actionDataAction))
                continue;
            if (t.actionDataArg != paramName && t.actionDataArg != parameter->name.name) continue;
            // The stepper re-enters this table on every path, so report each pin once rather than
            // once per path.
            static std::set<std::tuple<cstring, cstring, cstring>> reported;
            if (reported.emplace(properties.tableName, actionName, paramName).second) {
                printInfo("[CP annotation] %1%: pinning action data %2%(%3%) = %4% (%5%)",
                          properties.tableName, actionName, paramName, t.value, c->ref);
            }
            // A bool parameter needs a BoolLiteral - Constant::get would mint a bool-typed integer
            // constant, which has no bitvector translation. An `enum bit<N>` parameter needs no
            // such care: MidEnd::addDefaultPasses runs P4::EliminateSerEnums, which rewrites the
            // parameter's Type_Name to the underlying Type_Bits before symbolic execution ever
            // sees it, so no Type_SerEnum reaches this point on any target.
            if (parameter->type->is<IR::Type_Boolean>()) {
                return new IR::Equ(actionArg, IR::BoolLiteral::get(t.value != 0));
            }
            return new IR::Equ(actionArg, IR::Constant::get(actionArg->type, t.value));
        }
    }
    return nullptr;
}

void TableStepper::setTableDefaultEntries(
    const std::vector<const IR::ActionListElement *> &tableActionList) {
    // --cp-annotation `default_action(T) == A`: the controller installs A, so a fork into any other
    // action describes a device configuration that does not exist. The FILTER lives here, not in a
    // target's evalTargetTable, because the clause is not architecture-specific: tofino,
    // tofino-v1model and PNA all reach their keyless path through this function and inherit it,
    // while a target keeps only the DECISION to override (bmv2 refuses to override outside STF).
    // It runs BEFORE the cross-phase pin below so the two agree by construction - Phase 1's
    // recorded default action came out of this very filter.
    std::vector<const IR::ActionListElement *> annotatedActions;
    if (const auto *ann = loadedCpAnnotation(); ann != nullptr) {
        for (const auto *action : tableActionList) {
            const auto *tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
            const cstring actionName = stepper->state.getP4Action(tableAction)->controlPlaneName();
            const auto *c = ann->defaultActionClause(properties.tableName, actionName);
            if (c == nullptr) {
                continue;
            }
            annotatedActions.push_back(action);
            // The stepper re-enters this table on every path, so report each (table, action) pair
            // once instead of thousands of identical lines.
            static std::set<std::pair<cstring, cstring>> reported;
            if (reported.emplace(properties.tableName, actionName).second) {
                printInfo("[CP annotation] %1%: installing annotated default action %2% (%3%)",
                          properties.tableName, actionName, c->ref);
            }
        }
    }
    // An annotation naming an action this table does not offer must not silence the table: fall
    // back to the full list rather than emitting no branch at all.
    const auto &actionList = annotatedActions.empty() ? tableActionList : annotatedActions;

    // Cross-phase control-plane consistency, constructive half. The device installs ONE default
    // action per table for every phase, but this function mints a fresh
    // `<table>_<action>_arg_<param>` symbol on each phase's own solver query, so the phases can
    // disagree on the default action's data. Only an ANNOTATION-BACKED value is carried over from
    // the recorded phase (the injector filters to those): it is a stated control-plane fact, and
    // the annotation filter/cpActionArgPin re-derive the same value in every phase anyway, so the
    // pin only restates what is already true. A value nobody constrained is deliberately NOT
    // carried -- propagating one phase's arbitrary model pick would invent a device configuration
    // -- and the ACTION CHOICE is likewise not forced here; both are checked after the fact by the
    // tampering executor, which drops a candidate whose phases contradict each other.
    const auto *pinnedDefault = pinnedDefaultActionFor(stepper->state, properties.tableName);
    for (const auto *action : actionList) {
        const auto *tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        const auto *actionType = stepper->state.getP4Action(tableAction);

        auto &nextState = stepper->state.clone();

        // We get the control plane name of the action we are calling.
        cstring actionName = actionType->controlPlaneName();

        // Synthesize arguments for the call based on the action parameters.
        const auto &parameters = actionType->parameters;
        auto *arguments = new IR::Vector<IR::Argument>();
        std::vector<ActionArg> ctrlPlaneArgs;
        const IR::Expression *cpPin = nullptr;
        for (const auto *parameter : *parameters) {
            // Synthesize a variable constant here that corresponds to a control plane argument.
            const auto &actionArg = ControlPlaneState::getTableActionArgument(
                properties.tableName, actionName, parameter->name, parameter->type);

            arguments->push_back(new IR::Argument(actionArg));
            // We also track the argument we synthesize for the control plane.
            // Note how we use the control plane name for the parameter here.
            ctrlPlaneArgs.emplace_back(parameter, actionArg);
            if (const auto *pin = cpActionArgPin(actionName, parameter, actionArg)) {
                cpPin = cpPin == nullptr ? pin : new IR::LAnd(cpPin, pin);
            }
            if (const auto *pin =
                    pinnedDefaultArgPin(pinnedDefault, actionName, parameter, actionArg)) {
                cpPin = cpPin == nullptr ? pin : new IR::LAnd(cpPin, pin);
            }
        }
        const auto *ctrlPlaneActionCall = new ActionCall(actionType, ctrlPlaneArgs);

        // We add the arguments to our action call, effectively creating a const entry call.
        auto *synthesizedAction = tableAction->clone();
        synthesizedAction->arguments = arguments;

        // Finally, add all the new rules to the execution stepper->state.
        auto *tableConfig = new TableConfig(table, {});
        // Add the action selector to the table. This signifies a slightly different implementation.
        tableConfig->addTableProperty("overriden_default_action"_cs, ctrlPlaneActionCall);
        nextState.addTestObject("tableconfigs"_cs, properties.tableName, tableConfig);

        // Update all the tracking variables for tables.
        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(
            new IR::MethodCallStatement(Util::SourceInfo(), synthesizedAction));
        // Some path selection strategies depend on looking ahead and collecting potential
        // nodes. If that is the case, apply the CoverableNodesScanner visitor.
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(SymbexOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(stepper->state);
            collector.updateNodeCoverage(actionType, coveredNodes);
        }
        nextState.set(getTableHitVar(table), IR::BoolLiteral::get(false));
        nextState.set(getTableActionVar(table), getTableActionString(tableAction));
        nextState.set(getActiveTableVar(), IR::StringLiteral::get(table->name));
        std::stringstream tableStream;
        tableStream << "Table Branch: " << properties.tableName;
        tableStream << "| Overriding default action: " << actionName;
        nextState.add(*new TraceEvents::Generic(tableStream.str()));
        nextState.replaceTopBody(&replacements);
        // An annotated (or cross-phase pinned) action-data pin becomes this branch's condition: the
        // branch is otherwise unconditional, and the pin has to constrain the synthesized argument
        // rather than merely be recorded alongside it.
        stepper->result->emplace_back(cpPin == nullptr ? std::optional<const IR::Expression *>()
                                                       : std::optional(cpPin),
                                      stepper->state, nextState, coveredNodes);
    }
}

void TableStepper::evalTableControlEntries(
    const std::vector<const IR::ActionListElement *> &tableActionList) {
    const auto *key = table->getKey();
    BUG_CHECK(key != nullptr, "An empty key list should have been handled earlier.");

    // First, we compute the hit condition to trigger this particular action call.
    TableMatchMap matches;
    const auto *hitCondition = computeHit(&matches);

    // Now we iterate over all table actions and create a path per table action.
    for (const auto *action : tableActionList) {
        // Create an execution state per action.
        auto &nextState = stepper->state.clone();
        // Grab the path from the method call.
        const auto *tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        // Try to find the action declaration corresponding to the path reference in the table.
        const auto *actionType = stepper->state.getP4Action(tableAction);

        // We get the control plane name of the action we are calling.
        cstring actionName = actionType->controlPlaneName();
        // Synthesize arguments for the call based on the action parameters.
        const auto &parameters = actionType->parameters;
        auto *arguments = new IR::Vector<IR::Argument>();
        std::vector<ActionArg> ctrlPlaneArgs;
        const IR::Expression *cpPin = nullptr;
        for (const auto *parameter : *parameters) {
            // Synthesize a variable constant here that corresponds to a control plane argument.
            const auto &actionArg = ControlPlaneState::getTableActionArgument(
                properties.tableName, actionName, parameter->name, parameter->type);
            arguments->push_back(new IR::Argument(actionArg));
            // We also track the argument we synthesize for the control plane.
            // Note how we use the control plane name for the parameter here.
            ctrlPlaneArgs.emplace_back(parameter, actionArg);
            if (const auto *pin = cpActionArgPin(actionName, parameter, actionArg)) {
                cpPin = cpPin == nullptr ? pin : new IR::LAnd(cpPin, pin);
            }
        }
        ActionCall ctrlPlaneActionCall(actionType, ctrlPlaneArgs);

        // We add the arguments to our action call, effectively creating a const entry call.
        auto *synthesizedAction = tableAction->clone();
        synthesizedAction->arguments = arguments;

        // Finally, add all the new rules to the execution stepper->state.
        auto tableRule =
            TableRule(matches, TestSpec::LOW_PRIORITY, ctrlPlaneActionCall, TestSpec::TTL);
        auto *tableConfig = new TableConfig(table, {tableRule});
        nextState.addTestObject("tableconfigs"_cs, properties.tableName, tableConfig);

        // Update all the tracking variables for tables.
        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(
            new IR::MethodCallStatement(Util::SourceInfo(), synthesizedAction));
        // Some path selection strategies depend on looking ahead and collecting potential
        // nodes. If that is the case, apply the CoverableNodesScanner visitor.
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(SymbexOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(stepper->state);
            collector.updateNodeCoverage(actionType, coveredNodes);
        }

        nextState.set(getTableHitVar(table), IR::BoolLiteral::get(true));
        nextState.set(getTableActionVar(table), getTableActionString(tableAction));
        nextState.set(getActiveTableVar(), IR::StringLiteral::get(table->name));

        std::stringstream tableStream;
        tableStream << "Table Branch: " << properties.tableName;
        bool isFirstKey = true;
        const auto &keyElements = key->keyElements;

        for (const auto *keyElement : keyElements) {
            if (isFirstKey) {
                tableStream << " | Key(s): ";
            } else {
                tableStream << ", ";
            }
            tableStream << keyElement->expression;
            isFirstKey = false;
        }
        tableStream << "| Chosen action: " << actionName;
        nextState.add(*new TraceEvents::Generic(tableStream.str()));
        nextState.replaceTopBody(&replacements);
        // The annotated action-data pin rides along with the hit condition: this entry is reachable
        // only when the table hits AND the controller supplied the assumed action data.
        stepper->result->emplace_back(
            cpPin == nullptr ? hitCondition : new IR::LAnd(hitCondition, cpPin), stepper->state,
            nextState, coveredNodes);
    }
}

void TableStepper::evalTaintedTable() {
    // If the table is not immutable, we just do not add any entry and execute the default action.
    if (!properties.tableIsImmutable) {
        addDefaultAction(std::nullopt);
        return;
    }
    std::vector<Continuation::Command> replacements;
    auto &nextState = stepper->state.clone();

    // If the table is immutable, we execute all the constant entries in its list.
    // We get the current value of the inUndefinedState property.
    auto currentTaint = stepper->state.getProperty<bool>("inUndefinedState"_cs);
    replacements.emplace_back(Continuation::PropertyUpdate("inUndefinedState"_cs, true));

    const auto *entries = table->getEntries();
    // Sometimes, there are no entries. Just return.
    if (entries == nullptr) {
        return;
    }
    auto entryVector = entries->entries;

    for (const auto &entry : entryVector) {
        const auto *action = entry->getAction();
        const auto *tableAction = action->checkedTo<IR::MethodCallExpression>();
        replacements.emplace_back(new IR::MethodCallStatement(Util::SourceInfo(), tableAction));
    }
    // Since we do not know which table action was selected because of the tainted key, we also
    // set the selected action variable tainted.
    const auto &tableActionVar = getTableActionVar(table);
    nextState.set(tableActionVar,
                  stepper->programInfo.createTargetUninitialized(tableActionVar->type, true));
    // We do not know whether this table was hit or not.
    auto hitVar = getTableHitVar(table);
    nextState.set(hitVar, stepper->programInfo.createTargetUninitialized(hitVar->type, true));
    // Set current active table
    auto activeTableVar = getActiveTableVar();
    nextState.set(activeTableVar,
                  stepper->programInfo.createTargetUninitialized(activeTableVar->type, true));

    // Reset the property to its previous stepper->state.
    replacements.emplace_back(Continuation::PropertyUpdate("inUndefinedState"_cs, currentTaint));
    nextState.replaceTopBody(&replacements);
    stepper->result->emplace_back(nextState);
}

bool TableStepper::resolveTableKeys() {
    auto propertyIdx = -1;
    const IR::Key *key = nullptr;
    for (propertyIdx = 0; propertyIdx < static_cast<int>(table->properties->properties.size());
         ++propertyIdx) {
        const auto *property = table->properties->properties.at(propertyIdx);
        if (property->name == "key") {
            key = property->value->checkedTo<IR::Key>();
            break;
        }
    }

    if (key == nullptr) {
        return false;
    }

    auto keyElements = key->keyElements;
    for (size_t keyIdx = 0; keyIdx < keyElements.size(); ++keyIdx) {
        const auto *keyElement = keyElements.at(keyIdx);
        const auto *keyExpr = keyElement->expression;
        if (!SymbolicEnv::isSymbolicValue(keyExpr)) {
            // Resolve all keys in the table.
            const auto *const p4Table = table;
            ExprStepper::stepToSubexpr(
                keyExpr, stepper->result, stepper->state,
                [p4Table, propertyIdx, keyIdx](const Continuation::Parameter *v) {
                    // We have to clone a whole bunch of nodes first.
                    auto *clonedTable = p4Table->clone();
                    auto *clonedTableProperties = p4Table->properties->clone();
                    auto *properties = &clonedTableProperties->properties;
                    auto *propertyValue = properties->at(propertyIdx)->clone();
                    const auto *key = clonedTable->getKey();
                    CHECK_NULL(key);
                    auto *newKey = key->clone();
                    auto *newKeyElement = newKey->keyElements[keyIdx]->clone();
                    // Now bubble them up in reverse order.
                    // Replace the single (!) key in the table.
                    newKeyElement->expression = v->param;
                    newKey->keyElements[keyIdx] = newKeyElement;
                    propertyValue->value = newKey;
                    (*properties)[propertyIdx] = propertyValue;
                    clonedTable->properties = clonedTableProperties;
                    return Continuation::Return(clonedTable);
                });
            return true;
        }

        const auto *nameAnnot = keyElement->getAnnotation(IR::Annotation::nameAnnotation);
        // Some hidden tables do not have any key name annotations.
        BUG_CHECK(nameAnnot != nullptr || properties.tableIsImmutable,
                  "Non-constant table key without an annotation");
        cstring fieldName;
        if (nameAnnot != nullptr) {
            fieldName = nameAnnot->getName();
        }
        // It is actually possible to use a variety of types as key.
        // So we have to stay generic and produce a corresponding variable.
        cstring keyMatchType = keyElement->matchType->toString();
        // We can recover from taint for some match types, which is why we track taint.
        bool keyHasTaint = Taint::hasTaint(keyElement->expression);

        // Initialize the standard keyProperties.
        TableUtils::KeyProperties keyProperties(keyElement, fieldName, keyIdx, keyMatchType,
                                                keyHasTaint);
        properties.resolvedKeys.emplace_back(keyProperties);
    }
    return false;
}

void TableStepper::addDefaultAction(std::optional<const IR::Expression *> tableMissCondition) {
    const auto *defaultAction = table->getDefaultAction();
    const auto *tableAction = defaultAction->checkedTo<IR::MethodCallExpression>();
    const auto *actionType = stepper->state.getP4Action(tableAction);
    auto &nextState = stepper->state.clone();
    const auto *actionPath = tableAction->method->to<IR::PathExpression>();
    BUG_CHECK(actionPath, "Unknown formation of action '%1%' in table %2%", tableAction, table);

    std::vector<Continuation::Command> replacements;
    std::stringstream tableStream;
    tableStream << "Table Branch: " << properties.tableName;
    tableStream << " Choosing default action: " << actionPath;
    nextState.add(*new TraceEvents::Generic(tableStream.str()));
    replacements.emplace_back(new IR::MethodCallStatement(Util::SourceInfo(), tableAction));
    // Some path selection strategies depend on looking ahead and collecting potential
    // nodes.
    P4::Coverage::CoverageSet coveredNodes;
    if (requiresLookahead(SymbexOptions::get().pathSelectionPolicy)) {
        auto collector = CoverableNodesScanner(stepper->state);
        collector.updateNodeCoverage(actionType, coveredNodes);
    }
    nextState.set(getTableHitVar(table), IR::BoolLiteral::get(false));
    nextState.set(getTableActionVar(table), getTableActionString(tableAction));
    nextState.set(getActiveTableVar(), IR::StringLiteral::get(table->name));

    nextState.replaceTopBody(&replacements);
    stepper->result->emplace_back(tableMissCondition, stepper->state, nextState, coveredNodes);
}

void TableStepper::checkTargetProperties(
    const std::vector<const IR::ActionListElement *> & /*tableActionList*/) {}

void TableStepper::evalTargetTable(
    const std::vector<const IR::ActionListElement *> &tableActionList) {
    // If the table is not constant, the default action can always be executed.
    // This is because we can simply not enter any table entry.
    std::optional<const IR::Expression *> tableMissCondition = std::nullopt;
    // If the table is not immutable, we synthesize control plane entries and follow the paths.
    if (properties.tableIsImmutable) {
        // If the entries properties is constant it means the entries are fixed.
        // We cannot add or remove table entries.
        tableMissCondition = evalTableConstEntries();
    } else {
        evalTableControlEntries(tableActionList);
    }

    // Add the default action.
    addDefaultAction(tableMissCondition);
}

bool TableStepper::eval() {
    // Resolve any non-symbolic table keys. The function returns true when a key needs replacement.
    if (resolveTableKeys()) {
        return false;
    }
    // Mark the table itself as visited so that coverage checks (e.g., state-dependency
    // chain allCovered) can detect that this table was applied, which is more natural
    // than tracking the IR::Key node or scanning MethodCallStatement visitors.
    stepper->state.markVisited(table);
    // Gather the list of executable actions. This does not include default actions, for example.
    const auto tableActionList = TableUtils::buildTableActionList(*table);

    checkTargetProperties(tableActionList);

    // If the table key is tainted, the control plane entries do not really matter.
    // Assume that any action can be executed.
    // Important: This should follow the immutability check.
    // This is because the taint behavior may differ with constant entries.
    if (properties.tableIsTainted) {
        evalTaintedTable();
        return false;
    }

    evalTargetTable(tableActionList);

    return false;
}

TableStepper::TableStepper(ExprStepper *stepper, const IR::P4Table *table)
    : stepper(stepper), table(table) {
    properties.tableName = table->controlPlaneName();
    for (size_t index = 0; index < table->getActionList()->size(); index++) {
        const auto *action = table->getActionList()->actionList.at(index);
        properties.actionIdMap.emplace(action->controlPlaneName(), index);
    }

    // Set the appropriate properties when the table is immutable, meaning it has constant entries.
    TableUtils::checkTableImmutability(*table, properties);

    // If the table is in the set of entities to skip, we set it immutable.
    // Symbex will not add a control plane entry for this table.
    auto &skipped = SymbexOptions::get().skippedControlPlaneEntities;
    if (skipped.find(properties.tableName) != skipped.end()) {
        properties.tableIsImmutable = true;
    }
}

}  // namespace P4::P4Tools::Symbex
