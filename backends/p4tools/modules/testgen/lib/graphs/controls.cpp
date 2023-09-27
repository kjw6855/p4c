/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "backends/p4tools/modules/testgen/lib/graphs/controls.h"

#include <iostream>

#include <boost/graph/graphviz.hpp>

#include "frontends/p4/methodInstance.h"
#include "frontends/p4/tableApply.h"
#include "backends/p4tools/modules/testgen/lib/graphs/graphs.h"
#include "lib/log.h"
#include "lib/nullstream.h"

namespace P4Tools::P4Testgen {

using Graph = ControlGraphs::Graph;

void ControlGraphs::calc_ball_larus_on_graphs() {
    for (auto g_ : controlGraphsArray) {
        g = g_;
        calc_ball_larus();
    }
}

Graph *ControlGraphs::ControlStack::pushBack(Graph &currentSubgraph, const cstring &name) {
    auto &newSubgraph = currentSubgraph.create_subgraph();
    auto fullName = getName(name);
    boost::get_property(newSubgraph, boost::graph_name) = "cluster" + fullName;
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["label"] =
        boost::get_property(currentSubgraph, boost::graph_name) +
        (fullName != "" ? "." + fullName : fullName);
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["fontsize"] = "22pt";
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["style"] = "bold";
    names.push_back(name);
    subgraphs.push_back(&newSubgraph);
    return getSubgraph();
}

Graph *ControlGraphs::ControlStack::popBack() {
    names.pop_back();
    subgraphs.pop_back();
    return getSubgraph();
}

Graph *ControlGraphs::ControlStack::getSubgraph() const {
    return subgraphs.empty() ? nullptr : subgraphs.back();
}

cstring ControlGraphs::ControlStack::getName(const cstring &name) const {
    std::stringstream sstream;
    for (auto &n : names) {
        if (n != "") sstream << n << ".";
    }
    sstream << name;
    return cstring(sstream);
}

bool ControlGraphs::ControlStack::isEmpty() const { return subgraphs.empty(); }

using vertex_t = ControlGraphs::vertex_t;

ControlGraphs::ControlGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap, TestCase &testCase)
    : refMap(refMap), typeMap(typeMap), testCase(testCase) {
    visitDagOnce = false;
}

void ControlGraphs::resetTestCase(TestCase &newTestCase) {
    testCase = newTestCase;
}

bool ControlGraphs::preorder(const IR::PackageBlock *block) {
    for (auto it : block->constantValue) {
        if (!it.second) continue;
        if (it.second->is<IR::ControlBlock>()) {
            auto name = it.second->to<IR::ControlBlock>()->container->name;
            LOG1("Generating graph for top-level control " << name);

            Graph *g_ = new Graph();
            g = g_;
            instanceName = std::nullopt;
            boost::get_property(*g_, boost::graph_name) = name;
            BUG_CHECK(controlStack.isEmpty(), "Invalid control stack state");
            g = controlStack.pushBack(*g_, "");
            start_v = add_vertex("__START__", nullptr, VertexType::OTHER);
            exit_v = add_vertex("__EXIT__", nullptr, VertexType::OTHER);
            parents = {{start_v, new EdgeUnconditional()}};
            visit(it.second->getNode());

            for (auto parent : parents) {
                add_edge(parent.first, exit_v, parent.second->label());
            }
            BUG_CHECK((*g_).is_root(), "Invalid graph");
            controlStack.popBack();
            controlGraphsArray.push_back(g_);
        } else if (it.second->is<IR::PackageBlock>()) {
            visit(it.second->getNode());
        }
    }
    return false;
}

bool ControlGraphs::preorder(const IR::ControlBlock *block) {
    visit(block->container);
    return false;
}

bool ControlGraphs::preorder(const IR::P4Control *cont) {
    bool doPop = false;
    // instanceName == std::nullopt <=> top level
    if (instanceName != std::nullopt) {
        g = controlStack.pushBack(*g, instanceName.value());
        doPop = true;
    }
    return_parents.clear();
    visit(cont->body);
    merge_other_statements_into_vertex();

    parents.insert(parents.end(), return_parents.begin(), return_parents.end());
    return_parents.clear();
    if (doPop) g = controlStack.popBack();
    return false;
}

bool ControlGraphs::preorder(const IR::BlockStatement *statement) {
    for (const auto component : statement->components) visit(component);
    merge_other_statements_into_vertex();

    return false;
}

bool ControlGraphs::preorder(const IR::IfStatement *statement) {
    std::stringstream sstream;
    statement->condition->dbprint(sstream);
    auto v = add_and_connect_vertex(cstring(sstream), statement, VertexType::CONDITION);

    Parents new_parents;
    parents = {{v, new EdgeIf(EdgeIf::Branch::TRUE)}};
    visit(statement->ifTrue);
    merge_other_statements_into_vertex();

    new_parents.insert(new_parents.end(), parents.begin(), parents.end());
    parents = {{v, new EdgeIf(EdgeIf::Branch::FALSE)}};
    if (statement->ifFalse != nullptr) {
        visit(statement->ifFalse);
        merge_other_statements_into_vertex();
    }
    new_parents.insert(new_parents.end(), parents.begin(), parents.end());
    parents = new_parents;
    return false;
}

bool ControlGraphs::preorder(const IR::SwitchStatement *statement) {
    auto tbl = P4::TableApplySolver::isActionRun(statement->expression, refMap, typeMap);
    vertex_t v;
    // special case for action_run
    std::stringstream sstream;
    if (tbl == nullptr) {
        statement->expression->dbprint(sstream);
    } else {
        visit(tbl);
        sstream << "switch: action_run";
    }
    v = add_and_connect_vertex(cstring(sstream), statement, VertexType::SWITCH);

    Parents new_parents;
    parents = {};
    bool hasDefault{false};
    for (auto scase : statement->cases) {
        parents.emplace_back(v, new EdgeSwitch(scase->label));
        if (scase->statement != nullptr) {
            visit(scase->statement);
            merge_other_statements_into_vertex();
            new_parents.insert(new_parents.end(), parents.begin(), parents.end());
            parents.clear();
        }
        if (scase->label->is<IR::DefaultExpression>()) {
            hasDefault = true;
            break;
        }
    }
    // TODO(antonin): do not add default statement for action_run if all actions
    // are present
    if (!hasDefault)
        new_parents.emplace_back(v, new EdgeSwitch(new IR::DefaultExpression()));
    else
        new_parents.insert(new_parents.end(), parents.begin(), parents.end());
    parents = new_parents;
    return false;
}

bool ControlGraphs::preorder(const IR::MethodCallStatement *statement) {
    auto instance = P4::MethodInstance::resolve(statement->methodCall, refMap, typeMap);

    if (instance->is<P4::ApplyMethod>()) {
        auto am = instance->to<P4::ApplyMethod>();
        if (am->object->is<IR::P4Table>()) {
            visit(am->object->to<IR::P4Table>());
        } else if (am->applyObject->is<IR::Type_Control>()) {
            if (am->object->is<IR::Parameter>()) {
                ::error(ErrorType::ERR_UNSUPPORTED_ON_TARGET,
                        "%1%: control parameters are not supported by this target", am->object);
                return false;
            }
            BUG_CHECK(am->object->is<IR::Declaration_Instance>(),
                      "Unsupported control invocation: %1%", am->object);
            auto instantiation = am->object->to<IR::Declaration_Instance>();
            instanceName = instantiation->controlPlaneName();
            auto type = instantiation->type;
            if (type->is<IR::Type_Name>()) {
                auto tn = type->to<IR::Type_Name>();
                auto decl = refMap->getDeclaration(tn->path, true);
                visit(decl->to<IR::P4Control>());
            }
        } else {
            BUG("Unsupported apply method: %1%", instance);
        }
    } else {
        statementsStack.push_back(statement);
    }
    return false;
}

bool ControlGraphs::preorder(const IR::AssignmentStatement *statement) {
    statementsStack.push_back(statement);
    return false;
}

bool ControlGraphs::preorder(const IR::ReturnStatement *) {
    merge_other_statements_into_vertex();

    return_parents.insert(return_parents.end(), parents.begin(), parents.end());
    parents.clear();
    return false;
}

bool ControlGraphs::preorder(const IR::ExitStatement *) {
    merge_other_statements_into_vertex();

    for (auto parent : parents) add_edge(parent.first, exit_v, parent.second->label());
    parents.clear();
    return false;
}

bool ControlGraphs::preorder(const IR::Key *key) {
    std::stringstream sstream;

    // Build key
    for (auto elVec : key->keyElements) {
        sstream << elVec->matchType->path->name.name << ": ";
        bool has_name = false;
        for (auto ann : elVec->annotations->annotations) {
            if (ann->toString() == "@name") {
                sstream << ann->getName();
                has_name = true;
                break;
            }
        }
        if (!has_name) sstream << elVec->expression->toString();
        sstream << "\\n";
    }

    auto v = add_and_connect_vertex(cstring(sstream), key, VertexType::KEY);

    parents = {{v, new EdgeUnconditional()}};

    return false;
}

bool ControlGraphs::preorder(const IR::P4Action *action) {
    visit(action->body);
    return false;
}

bool ControlGraphs::preorder(const IR::P4Table *table) {
    auto name = table->getName();

    auto v = add_and_connect_vertex(name, table, VertexType::TABLE);

    parents = {{v, new EdgeUnconditional()}};

    auto key = table->getKey();
    visit(key);

    Parents keyNode;
    keyNode.emplace_back(parents.back());

    Parents new_parents;

    auto actList = table->getActionList();
    if (!actList) {
        parents = new_parents;
        return false;
    }
    auto actions = actList->actionList;

    // 1. Get actions from testCase
    for (auto &entity : *testCase.mutable_entities()) {
        if (!entity.has_table_entry())
            continue;

        auto *entry = entity.mutable_table_entry();
        if (!entry->is_valid_entry())
            continue;

        if (entry->table_name() != table->controlPlaneName())
            continue;

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

        for (auto action : actions) {
            auto mce = action->expression->checkedTo<IR::MethodCallExpression>();
            auto resolved = P4::MethodInstance::resolve(mce, refMap, typeMap);

            if (!(resolved->is<P4::ActionCall>()))
                continue;

            auto ac = resolved->to<P4::ActionCall>();
            if (!(ac->action->is<IR::P4Action>()))
                continue;

            cstring actionName = ac->action->controlPlaneName();
            if (actionName != p4v1Action->action_name())
                continue;

            // FOUND
            parents = keyNode;
            // TODO: add entry as vertex
            auto v = add_and_connect_vertex(action->getName(), action, VertexType::ACTION);
            boost::put(&Graphs::Vertex::entry, *g, v, entry);
            parents = {{v, new EdgeUnconditional()}};
            visit(ac->action->to<IR::P4Action>());
            merge_other_statements_into_vertex();

            new_parents.insert(new_parents.end(), parents.begin(), parents.end());
            parents.clear();
            break;
        }
    }

    // 2. Add default action
    const auto *defaultAction = table->getDefaultAction();
    const auto mce = defaultAction->checkedTo<IR::MethodCallExpression>();
    auto resolved = P4::MethodInstance::resolve(mce, refMap, typeMap);
    if (resolved->is<P4::ActionCall>()) {
        auto ac = resolved->to<P4::ActionCall>();
        if (ac->action->is<IR::P4Action>()) {
            parents = keyNode;
            // TODO: add entry as vertex
            std::stringstream sstream;
            sstream << defaultAction;
            auto v = add_and_connect_vertex(cstring(sstream), defaultAction, VertexType::ACTION);
            parents = {{v, new EdgeUnconditional()}};
            visit(ac->action->to<IR::P4Action>());
            merge_other_statements_into_vertex();

            new_parents.insert(new_parents.end(), parents.begin(), parents.end());
            parents.clear();
        }
    }


    parents = new_parents;

    return false;
}

}  // namespace P4Tools::P4Testgen
