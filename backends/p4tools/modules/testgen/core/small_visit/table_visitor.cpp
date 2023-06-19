#include "backends/p4tools/modules/testgen/core/small_visit/table_visitor.h"

#include <algorithm>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/multiprecision/number.hpp>

#include "backends/p4tools/common/lib/constants.h"
#include "backends/p4tools/common/lib/symbolic_env.h"
#include "backends/p4tools/common/lib/trace_event_types.h"
#include "backends/p4tools/common/lib/util.h"
#include "backends/p4tools/common/lib/variables.h"
#include "ir/id.h"
#include "ir/indexed_vector.h"
#include "ir/irutils.h"
#include "ir/vector.h"
#include "lib/exceptions.h"
#include "lib/log.h"
#include "lib/null.h"
#include "lib/source_file.h"
#include "midend/coverage.h"

#include "backends/p4tools/modules/testgen/core/program_info.h"
#include "backends/p4tools/modules/testgen/core/small_visit/expr_visitor.h"
#include "backends/p4tools/modules/testgen/core/symbolic_executor/path_selection.h"
#include "backends/p4tools/modules/testgen/lib/collect_coverable_nodes.h"
#include "backends/p4tools/modules/testgen/lib/continuation.h"
#include "backends/p4tools/modules/testgen/lib/exceptions.h"
#include "backends/p4tools/modules/testgen/lib/execution_state.h"
#include "backends/p4tools/modules/testgen/lib/test_spec.h"
#include "backends/p4tools/modules/testgen/options.h"

namespace P4Tools::P4Testgen {

const ExecutionState *TableVisitor::getExecutionState() { return &visitor->state; }

const ProgramInfo *TableVisitor::getProgramInfo() { return &visitor->programInfo; }

ExprVisitor::Result TableVisitor::getResult() { return visitor->result; }

const IR::StateVariable &TableVisitor::getTableStateVariable(const IR::Type *type,
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

const IR::StateVariable &TableVisitor::getTableActionVar(const IR::P4Table *table) {
    auto numActions = table->getActionList()->size();
    const auto *type = IR::getBitTypeToFit(numActions);
    return getTableStateVariable(type, table, "*action");
}

const IR::StateVariable &TableVisitor::getTableHitVar(const IR::P4Table *table) {
    return getTableStateVariable(IR::Type::Boolean::get(), table, "*hit");
}

const IR::StateVariable &TableVisitor::getTableKeyReadVar(const IR::P4Table *table, int keyIdx) {
    const auto *key = table->getKey()->keyElements.at(keyIdx);
    return getTableStateVariable(key->expression->type, table, "*keyRead", keyIdx);
}

const IR::StateVariable &TableVisitor::getTableReachedVar(const IR::P4Table *table) {
    return getTableStateVariable(IR::Type::Boolean::get(), table, "*reached");
}

const IR::Expression *TableVisitor::computeTargetMatchType(
    ExecutionState &nextState, const TableUtils::KeyProperties &keyProperties,
    TableMatchMap *matches, const IR::Expression *hitCondition) {
    const IR::Expression *keyExpr = keyProperties.key->expression;
    // Create a new variable constant that corresponds to the key expression.
    cstring keyName = properties.tableName + "_key_" + keyProperties.name;
    const auto *ctrlPlaneKey = nextState.createSymbolicVariable(keyExpr->type, keyName);

    if (keyProperties.matchType == P4Constants::MATCH_KIND_EXACT) {
        hitCondition = new IR::LAnd(hitCondition, new IR::Equ(keyExpr, ctrlPlaneKey));
        matches->emplace(keyProperties.name, new Exact(keyProperties.key, ctrlPlaneKey));
        return hitCondition;
    }
    if (keyProperties.matchType == P4Constants::MATCH_KIND_TERNARY) {
        cstring maskName = properties.tableName + "_mask_" + keyProperties.name;
        const IR::Expression *ternaryMask = nullptr;
        // We can recover from taint by inserting a ternary match that is 0.
        if (keyProperties.isTainted) {
            ternaryMask = IR::getConstant(keyExpr->type, 0);
            keyExpr = ternaryMask;
        } else {
            ternaryMask = nextState.createSymbolicVariable(keyExpr->type, maskName);
        }
        matches->emplace(keyProperties.name,
                         new Ternary(keyProperties.key, ctrlPlaneKey, ternaryMask));
        return new IR::LAnd(hitCondition, new IR::Equ(new IR::BAnd(keyExpr, ternaryMask),
                                                      new IR::BAnd(ctrlPlaneKey, ternaryMask)));
    }
    if (keyProperties.matchType == P4Constants::MATCH_KIND_LPM) {
        const auto *keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
        auto keyWidth = keyType->width_bits();
        cstring maskName = properties.tableName + "_lpm_prefix_" + keyProperties.name;
        const IR::Expression *maskVar = nextState.createSymbolicVariable(keyExpr->type, maskName);
        // The maxReturn is the maximum vale for the given bit width. This value is shifted by
        // the mask variable to create a mask (and with that, a prefix).
        auto maxReturn = IR::getMaxBvVal(keyWidth);
        auto *prefix = new IR::Sub(IR::getConstant(keyType, keyWidth), maskVar);
        const IR::Expression *lpmMask = nullptr;
        // We can recover from taint by inserting a ternary match that is 0.
        if (keyProperties.isTainted) {
            lpmMask = IR::getConstant(keyExpr->type, 0);
            maskVar = lpmMask;
            keyExpr = lpmMask;
        } else {
            lpmMask = new IR::Shl(IR::getConstant(keyType, maxReturn), prefix);
        }
        matches->emplace(keyProperties.name, new LPM(keyProperties.key, ctrlPlaneKey, maskVar));
        return new IR::LAnd(
            hitCondition,
            new IR::LAnd(
                // This is the actual LPM match under the shifted mask (the prefix).
                new IR::Leq(maskVar, IR::getConstant(keyType, keyWidth)),
                // The mask variable shift should not be larger than the key width.
                new IR::Equ(new IR::BAnd(keyExpr, lpmMask), new IR::BAnd(ctrlPlaneKey, lpmMask))));
    }

    TESTGEN_UNIMPLEMENTED("Match type %s not implemented for table keys.", keyProperties.matchType);
}

const IR::Expression *TableVisitor::computeHit(ExecutionState &nextState, TableMatchMap *matches) {
    const IR::Expression *hitCondition = IR::getBoolLiteral(!properties.resolvedKeys.empty());
    for (auto keyProperties : properties.resolvedKeys) {
        hitCondition = computeTargetMatchType(nextState, keyProperties, matches, hitCondition);
    }
    return hitCondition;
}

const IR::Expression* TableVisitor::computeHitFromTestCase(const ::p4::v1::TableEntry& entry) {
    const auto* keys = table->getKey();
    const IR::Expression* hitCondition = IR::getBoolLiteral(true);

    for (const auto& match : entry.match()) {
        size_t idx = (size_t)(match.field_id() - 1);

        BUG_CHECK(idx < keys->keyElements.size(),
                "given idx %1% is out of size %2%",
                idx, keys->keyElements.size());
        const auto* key = keys->keyElements.at(idx);

        const auto *nameAnnot = key->getAnnotation("name");
        if (nameAnnot != nullptr) {
            if (nameAnnot->getName() != match.field_name()) {
                std::cout << "** Different match name: "
                    << match.field_name() << " (testCase) vs. "
                    << nameAnnot->getName() << " (table)"
                    << std::endl;
                return nullptr;
            }
        }

        const IR::Expression* keyExpr = key->expression;
        const auto* keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
        auto keyWidth = keyType->width_bits();
        if (match.has_range()) {
            const auto& matchRange = match.range();
            const auto* minExpr = Utils::getValExpr(matchRange.low(), keyWidth);
            const auto* maxExpr = Utils::getValExpr(matchRange.high(), keyWidth);
            hitCondition = new IR::LAnd(
                    hitCondition,
                    new IR::LAnd(new IR::Leq(minExpr, keyExpr), new IR::Leq(keyExpr, maxExpr)));

        } else if (match.has_ternary()) {
            const auto& matchTernary = match.ternary();
            const auto* valExpr = Utils::getValExpr(matchTernary.value(), keyWidth);
            const auto* maskExpr = Utils::getValExpr(matchTernary.mask(), keyWidth);

            hitCondition = new IR::LAnd(
                    hitCondition, new IR::Equ(new IR::BAnd(keyExpr, maskExpr),
                        new IR::BAnd(valExpr, maskExpr)));

        } else if (match.has_lpm()) {
            const auto& matchLpm = match.lpm();
            const auto* valExpr = Utils::getValExpr(matchLpm.value(), keyWidth);
            // get maxReturn
            auto maxReturn = IR::getMaxBvVal(matchLpm.prefix_len());

            // maskExpr = (maxReturn) (Shl; <<) (prefix_len:32)
            auto* maskExpr = new IR::Shl(IR::getConstant(keyType, maxReturn),
                    new IR::Sub(IR::getConstant(keyType, keyWidth),
                        IR::getConstant(keyType, matchLpm.prefix_len())));

            hitCondition = new IR::LAnd(
                    hitCondition, new IR::Equ(new IR::BAnd(keyExpr, maskExpr),
                        new IR::BAnd(valExpr, maskExpr)));

        } else if (match.has_exact()) {
            const auto& matchExact = match.exact();
            const auto* valExpr = Utils::getValExpr(matchExact.value(), keyWidth);

            hitCondition = new IR::LAnd(hitCondition, new IR::Equ(keyExpr, valExpr));

        } else {
            //XXX: UNSUPPORTED;
            BUG("Unknown type");
        }
    }

    return hitCondition;
}

void TableVisitor::setTableAction(ExecutionState &nextState,
                                  const IR::MethodCallExpression *actionCall) {
    // Figure out the index of the selected action within the table's action list.
    // TODO: Simplify this. We really only need to work with indexes and names for the
    // respective table.
    const auto &actionList = table->getActionList()->actionList;
    size_t actionIdx = 0;
    for (; actionIdx < actionList.size(); ++actionIdx) {
        // Expect the expression within the ActionListElement to be a MethodCallExpression.
        const auto *expr = actionList.at(actionIdx)->expression;
        const auto *curCall = expr->to<IR::MethodCallExpression>();
        BUG_CHECK(curCall, "Action at index %1% for table %2% is not a MethodCallExpression: %3%",
                  actionIdx, table, expr);

        // Stop looping if the current action matches the selected action.
        if (curCall->method->equiv(*actionCall->method)) {
            break;
        }
    }

    BUG_CHECK(actionIdx < actionList.size(), "%1%: not a valid action for table %2%", actionCall,
              table);
    // Store the selected action.
    const auto &tableActionVar = getTableActionVar(table);
    nextState.set(tableActionVar, IR::getConstant(tableActionVar.type, actionIdx));
}

const IR::Expression *TableVisitor::evalTableConstEntries() {
    const IR::Expression *tableMissCondition = IR::getBoolLiteral(true);

    const auto *key = table->getKey();
    BUG_CHECK(key != nullptr, "An empty key list should have been handled earlier.");

    const auto *entries = table->getEntries();
    // Sometimes, there are no entries. Just return.
    if (entries == nullptr) {
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
        const auto *actionType = visitor->state.getP4Action(tableAction);
        auto &nextState = visitor->state.clone();
        nextState.markVisited(entry);
        // We need to set the table action in the state for eventual switch action_run hits.
        // We also will need it for control plane table entries.
        setTableAction(nextState, tableAction);
        // Compute the table key for a constant entry
        const auto *hitCondition = TableUtils::computeEntryMatch(*table, *entry, *key);

        // Update all the tracking variables for tables.
        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(new IR::MethodCallStatement(Util::SourceInfo(), tableAction));
        nextState.set(getTableHitVar(table), IR::getBoolLiteral(true));
        nextState.set(getTableReachedVar(table), IR::getBoolLiteral(true));
        // Some path selection strategies depend on looking ahead and collecting potential
        // statements. If that is the case, apply the CoverableNodesScanner visitor.
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(TestgenOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(visitor->state);
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
        visitor->result->emplace_back(new IR::LAnd(tableMissCondition, hitCondition),
                                      visitor->state, nextState, coveredNodes);
        tableMissCondition = new IR::LAnd(new IR::LNot(hitCondition), tableMissCondition);
    }
    return tableMissCondition;
}

void TableVisitor::setTableDefaultEntries(
    const std::vector<const IR::ActionListElement *> &tableActionList) {
    for (const auto *action : tableActionList) {
        const auto *tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        const auto *actionType = visitor->state.getP4Action(tableAction);

        auto &nextState = visitor->state.clone();

        // We get the control plane name of the action we are calling.
        cstring actionName = actionType->controlPlaneName();
        // Synthesize arguments for the call based on the action parameters.
        const auto &parameters = actionType->parameters;
        auto *arguments = new IR::Vector<IR::Argument>();
        std::vector<ActionArg> ctrlPlaneArgs;
        for (size_t argIdx = 0; argIdx < parameters->size(); ++argIdx) {
            const auto *parameter = parameters->getParameter(argIdx);
            // Synthesize a variable constant here that corresponds to a control plane argument.
            // We get the unique name of the table coupled with the unique name of the action.
            // Getting the unique name is needed to avoid generating duplicate arguments.
            cstring paramName =
                properties.tableName + "_arg_" + actionName + std::to_string(argIdx);
            const auto &actionArg = nextState.createSymbolicVariable(parameter->type, paramName);
            arguments->push_back(new IR::Argument(actionArg));
            // We also track the argument we synthesize for the control plane.
            // Note how we use the control plane name for the parameter here.
            ctrlPlaneArgs.emplace_back(parameter, actionArg);
        }
        const auto *ctrlPlaneActionCall = new ActionCall(actionType, ctrlPlaneArgs);

        // We add the arguments to our action call, effectively creating a const entry call.
        auto *synthesizedAction = tableAction->clone();
        synthesizedAction->arguments = arguments;

        // We need to set the table action in the state for eventual switch action_run hits.
        // We also will need it for control plane table entries.
        setTableAction(nextState, tableAction);

        // Finally, add all the new rules to the execution visitor->state.
        auto *tableConfig = new TableConfig(table, {});
        // Add the action selector to the table. This signifies a slightly different implementation.
        tableConfig->addTableProperty("overriden_default_action", ctrlPlaneActionCall);
        nextState.addTestObject("tableconfigs", properties.tableName, tableConfig);

        // Update all the tracking variables for tables.
        std::vector<Continuation::Command> replacements;
        replacements.emplace_back(
            new IR::MethodCallStatement(Util::SourceInfo(), synthesizedAction));
        // Some path selection strategies depend on looking ahead and collecting potential
        // statements. If that is the case, apply the CoverableNodesScanner visitor.
        P4::Coverage::CoverageSet coveredNodes;
        if (requiresLookahead(TestgenOptions::get().pathSelectionPolicy)) {
            auto collector = CoverableNodesScanner(visitor->state);
            collector.updateNodeCoverage(actionType, coveredNodes);
        }
        nextState.set(getTableHitVar(table), IR::getBoolLiteral(false));
        nextState.set(getTableReachedVar(table), IR::getBoolLiteral(true));
        std::stringstream tableStream;
        tableStream << "Table Branch: " << properties.tableName;
        tableStream << "| Overriding default action: " << actionName;
        nextState.add(*new TraceEvents::Generic(tableStream.str()));
        nextState.replaceTopBody(&replacements);
        visitor->result->emplace_back(std::nullopt, visitor->state, nextState, coveredNodes);
    }
}

// Verify mutant actionName and paramName/Val
bool TableVisitor::verifyAction(const ::p4::v1::Action &p4v1Action,
        const std::vector<const IR::ActionListElement *> tableActionList) {

    bool found = false;

    for (size_t idx = 0; idx < tableActionList.size(); idx++) {
        const auto* action = tableActionList.at(idx);
        const auto *tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        const auto* actionType = visitor->state.getP4Action(tableAction);
        CHECK_NULL(actionType);
        auto actionName = actionType->controlPlaneName();
        if (actionName != p4v1Action.action_name())
            continue;

        // action found
        found = true;
        for (auto& param : p4v1Action.params()) {
            // check param_name comparison
            size_t argIdx = (size_t)(param.param_id() - 1);
            const auto* parameter = actionType->parameters->getParameter(argIdx);
            /* [INVALID] param.param_name */
            if (parameter->controlPlaneName() != param.param_name()) {
                LOG_FEATURE("small_visit", 4, "Param name diff: "
                        << param.param_name() << " (TestCase) vs. "
                        << parameter->controlPlaneName() << " (Table)");

                return false;
            }

            /* [INVALID] param.value */
            const auto* paramType = parameter->type->checkedTo<IR::Type_Bits>();
            auto paramWidth = paramType->width_bits();
            if (Utils::getVal(param.value(), paramWidth) > IR::getMaxBvVal(paramWidth)) {
                LOG_FEATURE("small_visit", 4, "Param value range: "
                        << param.value() << " (TestCase) vs. |bit<"
                        << paramWidth << ">| (Table)");

                return false;
            }
        }
    }

    return found;
}

// Verify mutant match.field_name and value
bool TableVisitor::verifyMatch(const ::p4::v1::FieldMatch &p4v1Match, const IR::Key *keys) {
    size_t idx = (size_t)(p4v1Match.field_id() - 1);

    BUG_CHECK(idx < keys->keyElements.size(),
            "given idx %1% is out of size %2%",
            idx, keys->keyElements.size());
    const auto* key = keys->keyElements.at(idx);

    const auto *nameAnnot = key->getAnnotation("name");
    if (nameAnnot != nullptr) {
        if (nameAnnot->getName() != p4v1Match.field_name()) {
            LOG_FEATURE("small_visit", 4, "Match name diff: "
                << p4v1Match.field_name() << " (TestCase) vs. "
                << nameAnnot->getName() << " (Table)");
            return false;
        }
    }

    auto keyMatchType = key->matchType->toString();
    const IR::Expression* keyExpr = key->expression;
    const auto* keyType = keyExpr->type->checkedTo<IR::Type_Bits>();
    auto keyWidth = keyType->width_bits();
    // TODO
    const auto maxVal = IR::getMaxBvVal(keyWidth);
    if (p4v1Match.has_range()) {
        if (keyMatchType != "range")        // XXX: BMv2 Match
            return false;

        const auto& matchRange = p4v1Match.range();
        const auto lowVal = Utils::getVal(matchRange.low(), keyWidth);
        const auto highVal = Utils::getVal(matchRange.high(), keyWidth);
        if (lowVal > highVal)
            return false;
        else if (highVal > maxVal)
            return false;

    } else if (p4v1Match.has_ternary()) {
        if (keyMatchType != P4Constants::MATCH_KIND_TERNARY)
            return false;

        const auto& matchTernary = p4v1Match.ternary();
        if (Utils::getVal(matchTernary.value(), keyWidth) > maxVal ||
                Utils::getVal(matchTernary.mask(), keyWidth) > maxVal)
            return false;

    } else if (p4v1Match.has_lpm()) {
        if (keyMatchType != P4Constants::MATCH_KIND_LPM)
            return false;

        const auto& matchLpm = p4v1Match.lpm();
        if (matchLpm.prefix_len() > keyWidth)
            return false;

        // TODO: check error, if value is larger than maxVal

    } else if (p4v1Match.has_exact()) {
        if (keyMatchType != P4Constants::MATCH_KIND_EXACT)
            return false;

        const auto& matchExact = p4v1Match.exact();
        if (Utils::getVal(matchExact.value(), keyWidth) > maxVal)
            return false;

    } else {
        LOG_FEATURE("small_visit", 4, "Match " << p4v1Match.field_name()
                << " - Unknown type" << std::endl);
        return false;
    }

    return true;
}

void TableVisitor::verifyTableControlEntries(
    const std::vector<const IR::ActionListElement *> &tableActionList) {

    auto tableName = table->controlPlaneName();
    const auto *keys = table->getKey();

    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        auto *entry = entity.mutable_table_entry();
        if (entry->table_name() != tableName)
            continue;

        // Action & Parameter check first
        const auto entryAction = entry->action();
        if (entryAction.has_action() && !verifyAction(entryAction.action(), tableActionList)) {
            continue;

        } else if (entryAction.has_action_profile_action_set()) {
            const auto entryActionSet = entryAction.action_profile_action_set();
            bool foundProfileAction = true;
            for (const auto &p4v1Action : entryActionSet.action_profile_actions()) {
                if (!verifyAction(p4v1Action.action(), tableActionList)) {
                    foundProfileAction = false;
                    break;
                }
            }

            if (!foundProfileAction)
                continue;
        }

        // Match check
        bool found = true;
        for (const auto &p4v1Match : entry->match()) {
            if (!verifyMatch(p4v1Match, keys)) {
                found = false;
                break;
            }
        }

        if (found)
            entry->set_is_valid_entry(1);
    }
}

void TableVisitor::evalTableControlEntries(
    const std::vector<const IR::ActionListElement *> &tableActionList) {
    const auto *key = table->getKey();
    BUG_CHECK(key != nullptr, "An empty key list should have been handled earlier.");

    // TODO: sort entries in order of priority
    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        auto *entry = entity.mutable_table_entry();

        // Skip invalid entry!
        if (!entry->is_valid_entry()) {
            std::cout << "Invalid table entry in "
                << entry->table_name()
                << std::endl;
            continue;
        }

        if (entry->table_name() != properties.tableName) {
            std::cout << "Different table name: "
                << entry->table_name() << " (testCase) vs. "
                << properties.tableName << " (table)"
                << std::endl;
            continue;
        }

        /* XXX: NoAction? */
        if (!entry->has_action())
            continue;

        auto &nextState = visitor->state.clone();
        bool found = false;

        ::p4::v1::Action *p4v1Action;
        if (entry->action().has_action()) {
            p4v1Action = entry->mutable_action()->mutable_action();

        } else if (entry->action().has_action_profile_action_set()) {
            // TODO: multiple actions
            p4v1Action = entry->mutable_action()->mutable_action_profile_action_set()
                         ->mutable_action_profile_actions(0)->mutable_action();
        } else {
            continue;
        }

        for (size_t idx = 0; idx < tableActionList.size(); idx++) {
            const auto* action = tableActionList.at(idx);
            const auto* tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
            const auto* actionType = visitor->state.getP4Action(tableAction);
            CHECK_NULL(actionType);
            cstring actionName = actionType->controlPlaneName();

            if (actionName != p4v1Action->action_name())
                continue;

            P4::Coverage::CoverageSet coveredNodes;
            if (requiresLookahead(TestgenOptions::get().pathSelectionPolicy)) {
                auto collector = CoverableNodesScanner(visitor->state);
                collector.updateNodeCoverage(actionType, coveredNodes);
            }

            // FOUND!
            found = true;
            auto* arguments = new IR::Vector<IR::Argument>();
            std::vector<ActionArg> ctrlPlaneArgs;
            for (auto& param : p4v1Action->params()) {
                // TODO: check param_name comparison
                size_t argIdx = (size_t)(param.param_id() - 1);
                BUG_CHECK(argIdx < actionType->parameters->size(),
                        "given argIdx %1% is out of size %2%",
                        argIdx, actionType->parameters->size());
                const auto* parameter = actionType->parameters->getParameter(argIdx);
                const auto* paramType = parameter->type->checkedTo<IR::Type_Bits>();
                auto paramWidth = paramType->width_bits();

                // change to constant value
                const auto& paramExpr = Utils::getValExpr(param.value(), paramWidth);
                const auto& actionDataVar =  getTableStateVariable(parameter->type, table, "*actionData", idx, argIdx);
                //cstring paramName =
                //    properties.tableName + "_arg_" + actionName + std::to_string(argIdx);
                //const auto& actionArg = nextState->createZombieConst(parameter->type, paramName);
                //const auto* actionArgVar = new IR::Member(parameter->type, new IR::PathExpression("*"), paramName);
                nextState.set(actionDataVar, paramExpr);
                arguments->push_back(new IR::Argument(paramExpr));
                ctrlPlaneArgs.emplace_back(parameter, paramExpr);
            }

            auto* synthesizedAction = tableAction->clone();
            synthesizedAction->arguments = arguments;
            setTableAction(nextState, tableAction);

            std::vector<Continuation::Command> replacements;
            replacements.emplace_back(new IR::MethodCallStatement(synthesizedAction));

            // ??
            nextState.set(getTableHitVar(table), IR::getBoolLiteral(true));
            nextState.set(getTableReachedVar(table), IR::getBoolLiteral(true));
            nextState.replaceTopBody(&replacements);
            break;
        }

        if (found) {
            // XXX: should I put TraceEvent::Generic(tableStream.str())?
            const IR::Expression* hitCondition = computeHitFromTestCase(*entry);
            if (hitCondition != nullptr) {
                visitor->result->emplace_back(hitCondition, visitor->state, nextState);
            }
        }
    }
}

void TableVisitor::evalTaintedTable() {
    // If the table is not immutable, we just do not add any entry and execute the default action.
    if (!properties.tableIsImmutable) {
        addDefaultAction(std::nullopt);
        return;
    }
    std::vector<Continuation::Command> replacements;
    auto &nextState = visitor->state.clone();

    // If the table is immutable, we execute all the constant entries in its list.
    // We get the current value of the inUndefinedState property.
    auto currentTaint = visitor->state.getProperty<bool>("inUndefinedState");
    replacements.emplace_back(Continuation::PropertyUpdate("inUndefinedState", true));

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
                  visitor->programInfo.createTargetUninitialized(tableActionVar->type, true));
    // We do not know whether this table was hit or not.
    auto hitVar = getTableHitVar(table);
    nextState.set(hitVar, visitor->programInfo.createTargetUninitialized(hitVar->type, true));

    // Reset the property to its previous visitor->state.
    replacements.emplace_back(Continuation::PropertyUpdate("inUndefinedState", currentTaint));
    nextState.replaceTopBody(&replacements);
    visitor->result->emplace_back(nextState);
}

bool TableVisitor::resolveTableKeys() {
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

    const auto *state = getExecutionState();
    auto keyElements = key->keyElements;
    for (size_t keyIdx = 0; keyIdx < keyElements.size(); ++keyIdx) {
        const auto *keyElement = keyElements.at(keyIdx);
        const auto *keyExpr = keyElement->expression;
        if (!SymbolicEnv::isSymbolicValue(keyExpr)) {
            // Resolve all keys in the table.
            const auto *const p4Table = table;
            ExprVisitor::stepToSubexpr(
                keyExpr, visitor->result, visitor->state,
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

        const auto *nameAnnot = keyElement->getAnnotation("name");
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
        bool keyHasTaint = state->hasTaint(keyElement->expression);

        // Initialize the standard keyProperties.
        TableUtils::KeyProperties keyProperties(keyElement, fieldName, keyIdx, keyMatchType,
                                                keyHasTaint);
        properties.resolvedKeys.emplace_back(keyProperties);
    }
    return false;
}

void TableVisitor::addDefaultAction(std::optional<const IR::Expression *> tableMissCondition) {
    const auto *defaultAction = table->getDefaultAction();
    const auto *tableAction = defaultAction->checkedTo<IR::MethodCallExpression>();
    const auto *actionType = visitor->state.getP4Action(tableAction);
    auto &nextState = visitor->state.clone();
    // We need to set the table action in the state for eventual switch action_run hits.
    // We also will need it for control plane table entries.
    setTableAction(nextState, tableAction);
    const auto *actionPath = tableAction->method->checkedTo<IR::PathExpression>();

    std::vector<Continuation::Command> replacements;
    std::stringstream tableStream;
    tableStream << "Table Branch: " << properties.tableName;
    tableStream << " Choosing default action: " << actionPath;
    nextState.add(*new TraceEvents::Generic(tableStream.str()));
    replacements.emplace_back(new IR::MethodCallStatement(Util::SourceInfo(), tableAction));
    // Some path selection strategies depend on looking ahead and collecting potential
    // statements.
    P4::Coverage::CoverageSet coveredNodes;
    if (requiresLookahead(TestgenOptions::get().pathSelectionPolicy)) {
        auto collector = CoverableNodesScanner(visitor->state);
        collector.updateNodeCoverage(actionType, coveredNodes);
    }
    nextState.set(getTableHitVar(table), IR::getBoolLiteral(false));
    nextState.set(getTableReachedVar(table), IR::getBoolLiteral(true));
    nextState.replaceTopBody(&replacements);
    visitor->result->emplace_back(tableMissCondition, visitor->state, nextState, coveredNodes);
}

void TableVisitor::checkTargetProperties(
    const std::vector<const IR::ActionListElement *> & /*tableActionList*/) {}

void TableVisitor::evalTargetTable(
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

bool TableVisitor::eval() {
    // Set the appropriate properties when the table is immutable, meaning it has constant entries.
    TableUtils::checkTableImmutability(*table, properties);

    if (checkTable) {
        // Gather the list of executable actions. This does not include default actions, for example.
        const auto tableActionList = TableUtils::buildTableActionList(*table);

        checkTargetProperties(tableActionList);

        if (!properties.tableIsTainted && !properties.tableIsImmutable)
            verifyTableControlEntries(tableActionList);

        visitor->state.popBody();

    } else {
        // Resolve any non-symbolic table keys. The function returns true when a key needs replacement.
        if (resolveTableKeys()) {
            return false;
        }
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
    }

    return false;
}

TableVisitor::TableVisitor(ExprVisitor *visitor, const IR::P4Table *table, TestCase &testCase)
    : visitor(visitor), table(table), testCase(testCase) {
    properties.tableName = table->controlPlaneName();
    checkTable = visitor->checkTable;
}

}  // namespace P4Tools::P4Testgen
