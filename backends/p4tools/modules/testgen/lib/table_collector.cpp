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

bool TableCollector::preorder(const IR::P4Table *p4table) {
    //body.push(tmpBody);
    //tmpBody.clear();

    body.push(Continuation::Return(p4table));
    p4Tables.insert(p4table);
    const auto tableActionList = TableUtils::buildTableActionList(*p4table);
    for (size_t i = 0; i < tableActionList.size(); i++) {
        const auto* action = tableActionList.at(i);
        const auto* tableAction = action->expression->checkedTo<IR::MethodCallExpression>();
        actionNodes.emplace(tableAction);
    }
    return true;
}

const std::set<const IR::P4Table*> TableCollector::getP4TableSet() const { return p4Tables; }

const Continuation::Body &TableCollector::getP4Tables() const { return body; }

const P4::Coverage::CoverageSet &TableCollector::getActionNodes() const { return actionNodes; }

}  // namespace P4Tools::P4Testgen
