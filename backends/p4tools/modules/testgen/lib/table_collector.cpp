#include "backends/p4tools/modules/testgen/lib/table_collector.h"

#include <ostream>

#include "ir/dump.h"
#include "lib/log.h"
#include "backends/p4tools/common/lib/constants.h"
#include "backends/p4tools/common/lib/table_utils.h"
#include "backends/p4tools/common/lib/taint.h"
#include "backends/p4tools/modules/testgen/core/small_visit/table_visitor.h"

namespace P4Tools::P4Testgen {

TableExecutionState::TableExecutionState(const IR::P4Program *program)
    : AbstractExecutionState(program) {}

TableExecutionState &TableExecutionState::clone() const { return *new TableExecutionState(*this); }

const IR::Expression *TableExecutionState::get(const IR::StateVariable &var) const {
    return nullptr;
}

void TableExecutionState::set(const IR::StateVariable &var, const IR::Expression *value) {}

bool TableExecutionState::hasTaint(const IR::Expression *expr) const {
    return Taint::hasTaint(env.getInternalMap(), expr);
}

/**
 * TableCollector
 */
TableCollector::TableCollector() : body({}), tmpBody({}) {}

bool TableCollector::preorder(const IR::P4Control *p4control) {
    //tmpBody.clear();
    return true;
}

bool TableCollector::preorder(const IR::MethodCallStatement *methodCallStatement) {
    //tmpBody.push(methodCallStatement);
    return true;
}

bool TableCollector::preorder(const IR::P4Action *p4action) {
    actionNodes.emplace(p4action);
    return true;
}

bool TableCollector::preorder(const IR::P4Table *p4table) {
    //body.push(tmpBody);
    //tmpBody.clear();

    body.push(Continuation::Return(p4table));
    p4Tables.insert(p4table);
    const auto tableActionList = TableUtils::buildTableActionList(*p4table);
    const auto tableName = p4table->controlPlaneName();

    P4::Coverage::CoverageSet actionSet;
    for (size_t i = 0; i < tableActionList.size(); i++) {
        const auto* action = tableActionList.at(i);
        const auto* tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        actionSet.emplace(tableAction);
    }

    if (actionSet.size() > 0) {
        actionMap.insert(std::pair<cstring, P4::Coverage::CoverageSet>(tableName, actionSet));
    }

    return true;
}

const std::set<const IR::P4Table*> &TableCollector::getP4TableSet() const { return p4Tables; }

const Continuation::Body &TableCollector::getP4Tables() const { return body; }

const P4::Coverage::CoverageSet &TableCollector::getActionNodes() const { return actionNodes; }

const P4::Coverage::CoverageSet *TableCollector::getActions(cstring tableName) const {
    if (tableName.isNullOrEmpty()) {
        return &actionNodes;
    }

    auto pos = actionMap.find(tableName);
    if (pos != actionMap.end()) {
        return &(pos->second);
    }

    return nullptr;
}

void TableCollector::findP4Actions() {
    std::map<cstring, P4::Coverage::CoverageSet> localMap;

    for (const auto &actionPair : actionMap) {
        P4::Coverage::CoverageSet newSet;
        for (const auto *action : actionPair.second) {
            const auto *expr = action->checkedTo<IR::MethodCallExpression>();
            const auto *path = expr->method->checkedTo<IR::PathExpression>();
            auto name = path->path->name.name;

            for (auto *p4Action : actionNodes) {

                if (p4Action->checkedTo<IR::P4Action>()->name.name == name) {
                    newSet.emplace(p4Action);
                    break;
                }
            }
        }

        localMap.insert(std::pair<cstring, P4::Coverage::CoverageSet>(actionPair.first, newSet));
    }

    actionMap = localMap;
}

}  // namespace P4Tools::P4Testgen
