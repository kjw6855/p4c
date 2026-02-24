
#include "controls.h"

#include <boost/graph/graphviz.hpp>

#include "frontends/p4/methodInstance.h"
#include "frontends/p4/tableApply.h"
#include "graphs.h"
#include "lib/cstring.h"
#include "lib/log.h"
#include "lib/nullstream.h"

namespace P4::P4StateDependency {

using Graph = ControlGraphs::Graph;
const ControlGraphs::procedure_pair_t ControlGraphs::emptyProcedure;

Graph *ControlGraphs::ControlStack::pushBack(Graph &currentSubgraph, const cstring &name) {
    auto &newSubgraph = currentSubgraph.create_subgraph();
    auto fullName = getName(name);
    boost::get_property(newSubgraph, boost::graph_name) = "cluster" + fullName;
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["label"_cs] =
        boost::get_property(currentSubgraph, boost::graph_name) +
        (fullName != "" ? "." + fullName : fullName);
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["fontsize"_cs] = "22pt"_cs;
    boost::get_property(newSubgraph, boost::graph_graph_attribute)["style"_cs] = "bold"_cs;
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

ControlGraphs::ControlGraphs(P4::ReferenceMap *refMap, P4::TypeMap *typeMap,
                             std::filesystem::path graphsDir)
    : refMap(refMap), typeMap(typeMap), graphsDir(std::move(graphsDir)) {
    visitDagOnce = false;
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
            boost::get_property(*g_, boost::graph_name) = name.string();
            BUG_CHECK(controlStack.isEmpty(), "Invalid control stack state");
            g = controlStack.pushBack(*g_, cstring::empty);
            start_v = add_vertex("__START__"_cs, VertexFlags::ENTRY);
            exit_v = add_vertex("__EXIT__"_cs, VertexFlags::EXIT);
            parents = {{start_v, new EdgeUnconditional()}};
            visit(it.second->getNode());

            for (auto parent : parents) {
                add_edge(parent.first, exit_v, parent.second->name, EdgeType::CONTROL);
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
    //merge_other_statements_into_vertex();

    parents.insert(parents.end(), return_parents.begin(), return_parents.end());
    return_parents.clear();
    if (doPop) {
        g = controlStack.popBack();
    }
    return false;
}

bool ControlGraphs::preorder(const IR::BlockStatement *statement) {
    for (const auto component : statement->components) visit(component);
    //merge_other_statements_into_vertex();

    return false;
}

bool ControlGraphs::preorder(const IR::IfStatement *statement) {
    std::stringstream sstream;
    // If condition is either hit or miss
    auto hitTbl = P4::TableApplySolver::isHit(statement->condition, refMap, typeMap);
    auto missTbl = P4::TableApplySolver::isMiss(statement->condition, refMap, typeMap);
    if (hitTbl != nullptr) {
        visit_call(hitTbl->getName(), hitTbl);
        sstream << "hit";
    } else if (missTbl != nullptr) {
        visit_call(missTbl->getName(), missTbl);
        sstream << "miss";
    } else {
        statement->condition->dbprint(sstream);
    }

    auto v = add_and_connect_vertex(cstring(sstream), VertexFlags::CONDITION, statement);

    Parents new_parents;
    parents = {{v, new EdgeIf(true)}};
    visit(statement->ifTrue);

    new_parents.insert(new_parents.end(), parents.begin(), parents.end());
    parents = {{v, new EdgeIf(false)}};
    if (statement->ifFalse != nullptr) {
        visit(statement->ifFalse);
    }
    new_parents.insert(new_parents.end(), parents.begin(), parents.end());
    parents = new_parents;
    return false;
}

bool ControlGraphs::preorder(const IR::SwitchStatement *statement) {
    auto tbl = P4::TableApplySolver::isActionRun(statement->expression, refMap, typeMap);
    vertex_t v;
    // Special case for action_run.
    std::stringstream sstream;
    if (tbl == nullptr) {
        statement->expression->dbprint(sstream);
    } else {
        visit(tbl);
        sstream << "switch: action_run";
    }
    v = add_and_connect_vertex(cstring(sstream), VertexFlags::SWITCH, statement);

    Parents new_parents;
    parents = {};
    bool hasDefault{false};
    for (auto scase : statement->cases) {
        parents.emplace_back(v, new EdgeSwitch(scase->label));
        if (scase->statement != nullptr) {
            visit(scase->statement);
            //merge_other_statements_into_vertex();
            new_parents.insert(new_parents.end(), parents.begin(), parents.end());
            parents.clear();
        }
        if (scase->label->is<IR::DefaultExpression>()) {
            hasDefault = true;
            break;
        }
    }
    // TODO(antonin): do not add default statement for action_run if all actions
    // are present.
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
        if (auto table = am->object->to<IR::P4Table>()) {
            auto tableName = table->getName();
            visit_call(tableName, table);

        } else if (am->applyObject->is<IR::Type_Control>()) {
            if (am->object->is<IR::Parameter>()) {
                ::P4::error(ErrorType::ERR_UNSUPPORTED_ON_TARGET,
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
    } else if (instance->is<P4::ExternMethod>()) {
        std::stringstream sstream;
        statement->dbprint(sstream);
        auto vName = cstring(sstream);

        // Check if externs are stateful or not.
        bool isStateful = false;
        auto em = instance->to<P4::ExternMethod>();
        std::vector<std::string> statefulExternNames = {"Counter", "Meter", "Register", "RegisterAction", "register"};

        for (const std::string &name : statefulExternNames) {
            if (em->originalExternType->getName().name == name) {
                isStateful = true;
                break;
            }
        }

        VertexFlags flags = VertexFlags::STATEMENT;
        if (isStateful)
            flags |= VertexFlags::STATEFUL;
        auto v = add_and_connect_vertex(vName, flags, statement);
        LOG2("has ExternMethod:" << vName);
        parents = {{v, new EdgeUnconditional()}};
    } else {
        std::stringstream sstream;
        statement->dbprint(sstream);
        auto vName = cstring(sstream);

        auto v = add_and_connect_vertex(vName, VertexFlags::STATEMENT, statement);
        parents = {{v, new EdgeUnconditional()}};
    }
    return false;
}

bool ControlGraphs::preorder(const IR::BaseAssignmentStatement *statement) {
    std::stringstream sstream;
    statement->dbprint(sstream);
    auto vName = cstring(sstream);

    auto v = add_and_connect_vertex(vName, VertexFlags::STATEMENT, statement);
    parents = {{v, new EdgeUnconditional()}};
    return false;
}

bool ControlGraphs::preorder(const IR::ReturnStatement *) {
    //merge_other_statements_into_vertex();

    return_parents.insert(return_parents.end(), parents.begin(), parents.end());
    parents.clear();
    return false;
}

bool ControlGraphs::preorder(const IR::ExitStatement *) {
    //merge_other_statements_into_vertex();

    for (auto parent : parents) add_edge(parent.first, exit_v, parent.second->name, EdgeType::CONTROL);
    parents.clear();
    return false;
}

bool ControlGraphs::preorder(const IR::Key *key) {
    std::stringstream sstream;

    // Build key
    for (auto elVec : key->keyElements) {
        sstream << elVec->matchType->path->name.name << ": ";
        bool has_name = false;
        for (auto ann : elVec->annotations) {
            if (ann->toString() == "@name") {
                sstream << ann->getName();
                has_name = true;
                break;
            }
        }
        if (!has_name) sstream << elVec->expression->toString();
        sstream << "\\n";
    }

    auto v = add_and_connect_vertex(cstring(sstream), VertexFlags::KEY, key);

    parents = {{v, new EdgeUnconditional()}};

    return false;
}

bool ControlGraphs::preorder(const IR::P4Action *action) {
    auto pp = getProcedure(action);
    if (pp != emptyProcedure) {
        add_and_connect_vertex(pp.first, EdgeType::INTER_PROCEDURE);
        parents = {{pp.second, new EdgeProcedural()}};
        return false;
    }

    auto name = action->getName();
    auto start_v = add_and_connect_vertex(name,
            VertexFlags::ACTION | VertexFlags::ENTRY, action);
    parents = {{start_v, new EdgeUnconditional()}};
    visit(action->body);
    auto exit_v = add_and_connect_vertex("EXIT "_cs + name, VertexFlags::EXIT);
    parents = {{exit_v, new EdgeProcedural}};

    procedureGraphs[action] = {start_v, exit_v};

    return false;
}

bool ControlGraphs::preorder(const IR::P4Table *table) {
    auto pp = getProcedure(table);
    if (pp != emptyProcedure) {
        add_and_connect_vertex(pp.first, EdgeType::INTER_PROCEDURE);
        parents = {{pp.second, new EdgeProcedural()}};
        return false;
    }
    auto name = table->getName();

    // Check if it's add-on-miss
    bool isStateful = false;
    auto boolProp = table->getBooleanProperty("add_on_miss"_cs);
    if (boolProp != nullptr) {
        isStateful = boolProp->value;
    }

    VertexFlags flags = VertexFlags::TABLE | VertexFlags::ENTRY;
    if (isStateful)
        flags |= VertexFlags::STATEFUL;

    auto start_v = add_and_connect_vertex(name, flags, table);
    parents = {{start_v, new EdgeUnconditional()}};

    auto key = table->getKey();
    visit(key);

    Parents keyNode;
    keyNode.emplace_back(parents.back());

    Parents new_parents;

    auto actList = table->getActionList();
    if (actList) {
        auto actions = actList->actionList;
        for (auto action : actions) {
            parents = keyNode;
            auto actionName = action->getName();

            // visit_call if P4Action is valid;
            const IR::P4Action *actNode = nullptr;
            if (action->expression->is<IR::MethodCallExpression>()) {
                auto mce = action->expression->to<IR::MethodCallExpression>();
                // needed for visiting body of P4Action
                auto resolved = P4::MethodInstance::resolve(mce, refMap, typeMap);
                if (resolved->is<P4::ActionCall>()) {
                    auto ac = resolved->to<P4::ActionCall>();
                    actNode = ac->action->to<IR::P4Action>();
                }
            }

            if (actNode) {
                bool emptyAction = false;
                if (auto stmt = actNode->body->to<IR::BlockStatement>()) {
                    if (stmt->components.size() == 0) {
                        auto v = add_and_connect_vertex(actionName,
                                VertexFlags::ACTION, action);
                        parents = {{v, new EdgeUnconditional()}};
                        emptyAction = true;
                    }
                }

                if (!emptyAction)
                    visit_call(actionName, actNode);

            } else {
                auto v = add_and_connect_vertex(actionName, VertexFlags::ACTION, action);
                parents = {{v, new EdgeUnconditional()}};
            }

            new_parents.insert(new_parents.end(), parents.begin(), parents.end());
            parents.clear();
        }
    }

    parents = new_parents;
    auto exit_v = add_and_connect_vertex("EXIT "_cs + name, VertexFlags::EXIT);
    parents = {{exit_v, new EdgeProcedural}};

    procedureGraphs[table] = {start_v, exit_v};

    return false;
}

void ControlGraphs::visit_call(const cstring &name, const IR::Node *node) {
    // before visit
    auto call_v = add_and_connect_vertex("CALL "_cs + name, VertexFlags::CALL);
    parents = {{call_v, new EdgeProcedural()}};

    visit(node);

    // after visit
    auto ret_v = add_and_connect_vertex("RETURN "_cs + name, VertexFlags::RETURN);
    parents = {{ret_v, new EdgeUnconditional()}};
    add_edge(call_v, ret_v, cstring::empty, EdgeType::CALL_TO_RETURN);
}

}  // namespace P4::P4StateDependency
