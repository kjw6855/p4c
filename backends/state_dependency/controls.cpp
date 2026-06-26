
#include "controls.h"

#include <boost/graph/graphviz.hpp>

#include "frontends/p4/tableApply.h"
#include "graphs.h"
#include "lib/cstring.h"
#include "lib/log.h"
#include "lib/nullstream.h"

namespace P4::P4StateDependency {

using Graph = ControlGraphs::Graph;
const ControlGraphs::procedure_md_t ControlGraphs::emptyProcedure{};

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
                             std::filesystem::path graphsDir, cstring arch)
    : refMap(refMap),
      typeMap(typeMap),
      graphsDir(std::move(graphsDir)),
      arch(arch) {
    visitDagOnce = false;
}

bool isHeaderStruct(const IR::Type* type, const TypeMap* typeMap) {
    if (auto ts = type->to<IR::Type_Struct>()) {
        for (auto f : ts->fields) {
            auto ftype = typeMap->getType(f, true);
            if (ftype->is<IR::Type_Header>()) return true;
            // Recurse for nested structs
            if (isHeaderStruct(ftype, typeMap)) return true;
        }
    }
    return false;
}

bool ControlGraphs::isWrite(bool root_value) {
    const Context *ctxt = getContext();
    if (ctxt->child_index == 2) return true;
    return P4WriteContext::isWrite(root_value);
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
            graphName = name.string();
            boost::get_property(*g_, boost::graph_name) = graphName;
            procName = name.string();
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

void ControlGraphs::addApplyParams(const IR::ParameterList *params, Graphs::vertex_t startV,
        Graphs::vertex_t exitV) {
    for (auto *p : params->parameters) {
        const IR::Node *newEntryVar = nullptr;
        if (p->direction == IR::Direction::In) {
            newEntryVar = add_variable_in_vertex(p, startV, false);
        } else if (p->direction == IR::Direction::Out) {
            add_variable_in_vertex(p, exitV, true);
        } else if (p->direction == IR::Direction::InOut) {
            newEntryVar = add_variable_in_vertex(p, startV, false);
            add_variable_in_vertex(p, exitV, true);
        }

        auto pType = typeMap->getType(p, true);
        // Heuristics: add first encountered header struct variable to skip metadata
        if (newEntryVar != nullptr && isHeaderStruct(pType, typeMap) &&
                headerVarNames.find(graphName) == headerVarNames.end()) {
            LOG2("Header struct parameter: " << p);
            headerVarNames[graphName] = get_var_name(newEntryVar);
        }
    }
}

bool ControlGraphs::preorder(const IR::P4Control *cont) {
    bool doPop = false;
    // instanceName == std::nullopt <=> top level
    if (instanceName != std::nullopt) {
        g = controlStack.pushBack(*g, instanceName.value());
        doPop = true;
    }
    return_parents.clear();

    // TODO: find the variable name for header
    addApplyParams(cont->getApplyParameters(), start_v, exit_v);

    for (auto *decl : cont->controlLocals) {
        if (auto *dv = decl->to<IR::Declaration_Variable>()) {
            add_variable_in_vertex(dv, start_v, false);
        }
    }

    visit(cont->body);

    parents.insert(parents.end(), return_parents.begin(), return_parents.end());
    return_parents.clear();
    if (doPop) {
        g = controlStack.popBack();
    }
    return false;
}

bool ControlGraphs::preorder(const IR::P4Parser *parser) {
    // Whole-pipeline modeling: a parser is one IFDS procedure (its states are intra-procedural basic
    // blocks). It is only ever entered inside an active pipeline graph via the dummy-main's visit_call
    // (a later commit) — NEVER during the legacy per-control traversal (preorder(PackageBlock) returns
    // false and only visits ControlBlocks), so this is inert until then. Mirrors preorder(P4Action).
    BUG_CHECK(g != nullptr, "P4Parser %1% visited without an active graph (scope contract)", parser);
    auto name = parser->getName();
    auto oldLocalProcFlags = localProcFlags;
    // Parser apply params (hdr/meta/std_meta) are block-boundary variables threaded across pipeline
    // blocks as GLOBALS, not control-plane locals — keep localProcFlags clear so addApplyParams uses
    // add_variable_in_vertex (global) rather than the local path.
    localProcFlags = VertexFlags::NONE;

    // Procedure ENTRY (connected from the call-site parents) and a standalone EXIT created up front so
    // out/inout apply params (e.g. `out H hdr`) can register at it.
    auto start_v = add_and_connect_vertex(name, VertexFlags::ENTRY, parser);
    parents = {{start_v, new EdgeUnconditional()}};
    auto exit_v = add_vertex("EXIT "_cs + name, VertexFlags::EXIT);

    addApplyParams(parser->getApplyParameters(), start_v, exit_v);

    // Walk the state CFG from "start". preorder(PathExpression) auto-follows Type_State transitions,
    // so the whole reachable state graph is traversed and each state's assignments/extracts run through
    // the existing statement handlers (e.g. `meta.f = hdr.g` builds a hdr.g->meta.f dep edge via
    // preorder(BaseAssignmentStatement); extract goes through the generic extern path). Components chain
    // via `parents` from start_v. visitDagOnce cuts any residual loop (parsers are also unrolled in the
    // SD prep). `select(...)` keysets are visited as reads. Sound over-approximation for a may-analysis.
    // TODO(step 5): procedureGraphs[parser] = {start_v, exit_v, retVals} so the dummy-main can
    //               visit_call this parser as a pipeline-block procedure.
    auto oldstate = state;
    if (state == SKIPPING) state = NORMAL;
    const IR::ParserState *startState = nullptr;
    for (auto *s : parser->states) {
        if (s->name.name == "start") { startState = s; break; }
    }
    if (startState != nullptr) visit(startState);
    state = oldstate;

    // Connect terminal states (whatever the walk left in `parents`) to the procedure EXIT.
    for (auto &p : parents) add_edge(p.first, exit_v, p.second->name, EdgeType::CONTROL);
    parents = {{exit_v, new EdgeProcedural()}};
    localProcFlags = oldLocalProcFlags;
    return false;
}

bool ControlGraphs::preorder(const IR::BlockStatement *statement) {
    for (const auto component : statement->components) visit(component);

    return false;
}

bool ControlGraphs::preorder(const IR::IfStatement *statement) {
    std::stringstream sstream;
    // If condition is either hit or miss
    auto hitTbl = P4::TableApplySolver::isHit(statement->condition, refMap, typeMap);
    auto missTbl = P4::TableApplySolver::isMiss(statement->condition, refMap, typeMap);

    bool visitCond = false;
    if (hitTbl != nullptr) {
        visit_call(hitTbl->getName(), hitTbl);
        sstream << "hit";
    } else if (missTbl != nullptr) {
        visit_call(missTbl->getName(), missTbl);
        sstream << "miss";
    } else {
        visitCond = true;
        statement->condition->dbprint(sstream);
    }

    auto v = add_and_connect_vertex(cstring(sstream), VertexFlags::CONDITION, statement);
    auto prev_cur_v = cur_v;
    cur_v = v;
    if (visitCond) {
        auto oldState = state;
        state = READ_ONLY;
        visit(statement->condition);
        state = oldState;
    }
    cur_v = prev_cur_v;

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
        visit_call(tbl->getName(), tbl);
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

    std::vector<const IR::Node *> params;
    for (auto *p : *statement->methodCall->arguments) {
        if (auto *arg = p->to<IR::Argument>()) {
            params.push_back(arg->expression);
        } else {
            params.push_back(p);
        }
    }

    if (instance->is<P4::ActionCall>()) {
        auto ac = instance->to<P4::ActionCall>();
        auto actNode = ac->action->to<IR::P4Action>();
        auto actionName = actNode->getName();
        visit_call(actionName, actNode, VertexFlags::ACTION);

    } else if (instance->is<P4::ApplyMethod>()) {
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

        if (em->originalExternType->getName().name == "register") {
            if (em->method->name.name == "read" && params.size() >= 2) {
                // void read(out T result, in I index);
                visit_stateful(vName, statement, {params[1]}, SOFlags::READ, {params[0]},
                    em->object->getNode());
                return false;

            } else if (em->method->name.name == "write" && params.size() >= 2) {
                // void write(in I index, in T value);
                visit_stateful(vName, statement, {params[0]}, SOFlags::UPDATE, {params[1]},
                    em->object->getNode());
                return false;
            }
        } else if (em->originalExternType->getName().name == "Counter") {
            if (em->method->name.name == "count" && params.size() == 1) {
                // No data field
                visit_stateful(vName, statement, {params[0]}, SOFlags::UPDATE, {},
                    em->object->getNode());
                return false;
            }
        } else if (em->originalExternType->getName().name == "Meter") {
            if (em->method->name.name == "execute") {
                if (params.size() == 1) {
                    visit_stateful(vName, statement, {params[0]}, SOFlags::UPDATE, {},
                            em->object->getNode());
                } else if (params.size() == 2) {
                    visit_stateful(vName, statement, {params[0]}, SOFlags::UPDATE,
                            {params[1]}, em->object->getNode());
                }
                return false;
            }
        } else if (em->originalExternType->getName().name == "RegisterAction") {
            // TODO: support other methods?
            if (em->method->name.name == "execute" && instance->object) {
                auto obj = instance->object->getNode();
                if (!isInContext(obj)) {
                    std::stringstream sstream;
                    em->expr->method->dbprint(sstream);
                    auto extName = cstring(sstream);
                    visit_call(extName, obj, VertexFlags::SO_IDX, params, {},
                            em->object->getNode(), statement);
                    return false;
                }
            }
        }

        // Other externs...
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

        auto prev_cur_v = cur_v;
        cur_v = v;
        for (auto *p : *statement->methodCall->arguments) visit(p);
        cur_v = prev_cur_v;
    } else {
        std::stringstream sstream;
        statement->dbprint(sstream);
        auto vName = cstring(sstream);
        auto flags = VertexFlags::STATEMENT;
        if (auto *ec = instance->to<P4::ExternCall>()) {
            // add_entry
            if (ec->method->name.name == "add_entry") {
                BUG_CHECK(params.size() >= 2,
                        "add_entry requires more params: %1%",
                        params.size());
                // Key goes index
                visit_stateful(vName, statement, curKeyVars,
                        SOFlags::CREATE, {params[1]}, nullptr);
                return false;
            }

            // psa and tna set drop in meta.drop and meta.drop_ctl, respectively
            if (arch == "v1model") {
                // v1model specific extern calls
                if (ec->method->name.name == "mark_to_drop") {
                    flags |= VertexFlags::DROP;
                }
            } else if (arch == "pna") {
                // pna specific extern calls
                if (ec->method->name.name == "drop_packet") {
                    flags |= VertexFlags::DROP;
                } else if (ec->method->name.name == "send_to_port") {
                    // TODO: add special variable for egress_port
                }
            }
        }

        auto v = add_and_connect_vertex(vName, flags, statement);
        parents = {{v, new EdgeUnconditional()}};

        auto prev_cur_v = cur_v;
        cur_v = v;
        for (auto *p : *statement->methodCall->arguments) visit(p);
        cur_v = prev_cur_v;
    }
    return false;
}

bool ControlGraphs::preorder(const IR::MethodCallExpression *mc) {
    auto *instance = P4::MethodInstance::resolve(mc, refMap, typeMap);

    if (state == WRITE_ONLY) {
        BUG_CHECK(!isWrite(), "Method call in out or inout arg should have failed typechecking");
        return false;
    } else if (state == READ_ONLY) {
        if (!isRead()) return false;
    }

    auto oldstate = state;
    state = READ_ONLY;
    visit(mc->arguments);
    state = NORMAL;

    if (instance->to<P4::ActionCall>()) {
        BUG("ActionCall should be called in MethodCallStatement");

    } else if (auto *bi = instance->to<P4::BuiltInMethod>()) {
        if (bi->name == "isValid")
            state = READ_ONLY;
        else if (bi->name == "setValid" || bi->name == "setInvalid")
            state = WRITE_ONLY;
        else if (bi->name == "setValid" || bi->name == "setInvalid")
            state = WRITE_ONLY;
        else
            BUG("unknown BuiltInMethod: %s", mc);

        visit(mc->method);

    } else if (instance->object) {
        auto obj = instance->object->getNode();
        if (!isInContext(obj)) visit(obj);
    }

    state = WRITE_ONLY;
    visit(mc->arguments);
    state = oldstate;
    return false;
}

bool ControlGraphs::preorder(const IR::BaseAssignmentStatement *statement) {
    // Directly check if statement is RegisterAction to capture lvalue
    if (auto *rmce = statement->right->to<IR::MethodCallExpression>()) {
        auto instance = P4::MethodInstance::resolve(rmce, refMap, typeMap);
        if (auto *em = instance->to<P4::ExternMethod>()) {
            if (em->originalExternType->getName().name == "RegisterAction" &&
                    em->method->name.name == "execute" &&
                    instance->object) {
                // FOUND
                auto obj = instance->object->getNode();
                if (!isInContext(obj)) {
                    std::stringstream sstream;
                    em->expr->method->dbprint(sstream);
                    auto extName = cstring(sstream);
                    std::vector<const IR::Node *> params;
                    for (auto *p : *rmce->arguments) {
                        if (auto *arg = p->to<IR::Argument>())
                            params.push_back(arg->expression);
                        else
                            params.push_back(p);
                    }
                    visit_call(extName, obj, VertexFlags::SO_IDX,
                            params, {statement->left}, nullptr, statement);
                    return false;
                }
            }
        }
    }
    std::stringstream sstream;
    statement->dbprint(sstream);
    auto vName = cstring(sstream);

    auto v = add_and_connect_vertex(vName, VertexFlags::STATEMENT, statement);
    parents = {{v, new EdgeUnconditional()}};

    auto prev_cur_v = cur_v;
    cur_v = v;
    auto oldstate = state;
    state = NORMAL;
    visit(statement->right, "right", 1);
    visit(statement->left, "left", 0);
    state = oldstate;
    cur_v = prev_cur_v;

    return false;
}

bool ControlGraphs::preorder(const IR::ReturnStatement *) {
    return_parents.insert(return_parents.end(), parents.begin(), parents.end());
    parents.clear();
    return false;
}

const P4::ExternMethod *ControlGraphs::get_extern_method(const Visitor::Context *ctxt_) {
    auto ctxt = ctxt_;
    while (ctxt) {
        // Check only first encountered method/assignment call (execute())
        if (auto mcs = ctxt->node->to<IR::MethodCallStatement>()) {
            auto instance = P4::MethodInstance::resolve(mcs->methodCall, refMap, typeMap);
            if (auto em = instance->to<P4::ExternMethod>()) {
                if (em->originalExternType->getName().name == "RegisterAction" &&
                        em->method->name.name == "execute")
                    return em;
            }
            return nullptr;
        } else if (auto as = ctxt->node->to<IR::BaseAssignmentStatement>()) {
            if (auto *rmce = as->right->to<IR::MethodCallExpression>()) {
                auto instance = P4::MethodInstance::resolve(rmce, refMap, typeMap);
                if (auto em = instance->to<P4::ExternMethod>()) {
                    if (em->originalExternType->getName().name == "RegisterAction" &&
                            em->method->name.name == "execute")
                        return em;
                }
            }
            return nullptr;
        }
        ctxt = ctxt->parent;
    }

    return nullptr;
}

bool ControlGraphs::preorder(const IR::Declaration_Variable *v) {
    // Store variable only if it's in stateful statement
    if (localProcFlags == VertexFlags::NONE) return true;

    if (cur_v.has_value())
        add_local_variable_in_vertex(v, cur_v.value(), false);
    return false;
}

bool ControlGraphs::preorder(const IR::Function *fn) {
    if (auto *em = get_extern_method(getContext())) {
        std::stringstream sstream;
        em->expr->method->dbprint(sstream);
        auto vName = cstring(sstream);

        // Find stateful object from arguments
        auto obj = em->object->getNode();
        const IR::Node *statefulObj = nullptr;
        if (obj->is<IR::Declaration_Instance>()) {
            auto args = obj->to<IR::Declaration_Instance>()->arguments;
            if (args != nullptr && args->size() > 0) {
                auto regArg = args->front();
                if (regArg->is<IR::Argument>()) {
                    auto regExpr = regArg->to<IR::Argument>()->expression;
                    if (auto p = regExpr->to<IR::PathExpression>()) {
                        auto decl = refMap->getDeclaration(p->path, true);
                        if (decl != nullptr) {
                            statefulObj = decl->getNode();
                        }
                    }
                }
            }
        }

        VertexFlags flags = VertexFlags::ENTRY | VertexFlags::STATEFUL;
        auto start_v = add_and_connect_vertex(vName, flags, fn);
        (*g)[start_v].statefulObjectNode = statefulObj;
        parents = {{start_v, new EdgeUnconditional()}};

        auto next_v = start_v;

        if (fn->type->parameters->size() > 0) {
            sstream.str("");
            sstream << "INPUT: ";
            bool isInit = true;
            for (auto *p : *fn->type->parameters) {
                if (isInit) isInit = false;
                else sstream << ", ";
                p->dbprint(sstream);
            }

            // TODO: check duplicated fn in two start_v
            next_v = add_and_connect_vertex(cstring(sstream),
                    flags & ~VertexFlags::ENTRY, fn);
            parents = {{next_v, new EdgeUnconditional()}};
        }
        auto oldstate = state;
        if (state == SKIPPING) state = NORMAL;
        for (auto *p : *fn->type->parameters) {
            // register param could be output
            if (p->direction == IR::Direction::Out ||
                    p->direction == IR::Direction::InOut)
                add_local_variable_in_vertex(p, next_v, false);
        }
        state = oldstate;

        // Visit internal body
        auto oldLocalProcFlags = localProcFlags;    // nested..
        localProcFlags = VertexFlags::SO_DATA;
        auto prev_cur_v = cur_v;
        cur_v = next_v;
        visit(fn->body);
        cur_v = prev_cur_v;
        localProcFlags = oldLocalProcFlags;

        auto exit_v = add_and_connect_vertex("EXIT "_cs + vName,
                    VertexFlags::EXIT | VertexFlags::STATEFUL);
        (*g)[exit_v].statefulObjectNode = statefulObj;
        parents = {{exit_v, new EdgeProcedural}};
        oldstate = state;
        if (state == SKIPPING) state = NORMAL;

        // map Out retVar will be mapped to return values (e.g.., lvalue)
        std::vector<const IR::Node *> retVals;
        prev_cur_v = cur_v;
        cur_v = exit_v;
        for (auto *p : *fn->type->parameters) {
            // register param could be output
            if (p->direction == IR::Direction::Out) {
                if (auto *pe = p->to<IR::PathExpression>()) {
                    const IR::Node *retVal;
                    add_variables(pe, getContext(), true, &retVal);
                    retVals.push_back(retVal);
                } else {
                    retVals.push_back(add_local_variable_in_vertex(p, exit_v, true));
                }
            }

        }
        cur_v = prev_cur_v;
        state = oldstate;
        procedureGraphs[obj] = {start_v, exit_v, retVals};
        return false;
    }

    if (!cur_v.has_value()) return false;

    auto oldstate = state;
    if (state == SKIPPING) state = NORMAL;
    for (auto *p : *fn->type->parameters) {
        add_variable_in_vertex(p, cur_v.value(), true);
    }
    visit(fn->body);
    state = oldstate;
    return false;
}

bool ControlGraphs::preorder(const IR::ExitStatement *) {
    for (auto parent : parents) add_edge(parent.first, exit_v, parent.second->name, EdgeType::CONTROL);
    parents.clear();
    return false;
}

bool ControlGraphs::preorder(const IR::KeyElement *ke) {
    visit(ke->expression);
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

    auto prev_cur_v = cur_v;
    cur_v = v;
    auto oldstate = state;
    storeKeys = true;
    curKeyVars = {};
    state = READ_ONLY;      // store keys in useVars
    for (auto elVec : key->keyElements) visit(elVec);
    storeKeys = false;
    state = oldstate;
    cur_v = prev_cur_v;

    return false;
}

bool ControlGraphs::preorder(const IR::P4Action *action) {
    auto name = action->getName();
    auto flags = VertexFlags::ACTION;
    auto oldLocalProcFlags = localProcFlags;

    flags |= VertexFlags::ENTRY;
    localProcFlags = VertexFlags::ACTION_DATA;

    auto start_v = add_and_connect_vertex(name, flags, action);
    parents = {{start_v, new EdgeUnconditional()}};
    auto next_v = start_v;

    if (action->parameters->size() > 0) {
        // Create one more node to cover 0->param Edge
        std::stringstream sstream;
        sstream << "INPUT: ";
        bool isInit = true;
        for (auto *p : *action->parameters) {
            if (isInit) isInit = false;
            else sstream << ", ";
            p->dbprint(sstream);
        }
        // TODO: check duplicated action in two start_v
        next_v = add_and_connect_vertex(cstring(sstream),
                flags & ~VertexFlags::ENTRY, action);
        parents = {{next_v, new EdgeUnconditional()}};
    }
    // ActionParam is newly defined by control plane rules
    for (auto *p : *action->parameters) {
        add_local_variable_in_vertex(p, next_v, false);
    }

    visit(action->body);

    localProcFlags = oldLocalProcFlags;
    auto exit_v = add_and_connect_vertex("EXIT "_cs + name, VertexFlags::EXIT);
    parents = {{exit_v, new EdgeProcedural}};
    procedureGraphs[action] = {start_v, exit_v};

    return false;
}

bool ControlGraphs::preorder(const IR::P4Table *table) {
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

                if (!emptyAction) {
                    visit_call(actionName, actNode, VertexFlags::ACTION);
                    auto pp = getProcedure(actNode);
                    actionMaps[graphName][actionName] = pp.first;
                }

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

bool ControlGraphs::preorder(const IR::PathExpression *pe) {
    if (pe->type->is<IR::Type_State>()) {
        auto *d = resolveUnique(pe->path->name, P4::ResolutionType::Any);
        BUG_CHECK(d, "failed to resolve %s", pe);
        auto ps = d->to<IR::ParserState>();
        BUG_CHECK(ps, "%s is not a parser state", d);
        visit(ps);
        return false;
    }

    if (state == SKIPPING) return false;
    if (cur_v.has_value()) {
        if (isRead() && state != WRITE_ONLY)
            add_variables(pe, getContext(), true);
        if (isWrite() && state != READ_ONLY)
            add_variables(pe, getContext(), false);
    }

    return false;
}

void ControlGraphs::visit_stateful(const cstring &name, const IR::Node *node,
        std::vector<const IR::Node *> indices, SOFlags soFlags,
        std::vector<const IR::Node *> dataVals, const IR::Node *soObj) {
    VertexFlags flags = VertexFlags::STATEMENT | VertexFlags::STATEFUL;
    // Access idx first
    auto fv = add_and_connect_vertex("ACCESS "_cs + name,
            flags | VertexFlags::SO_IDX, node);
    auto &fvinfo = (*g)[fv];
    fvinfo.soFlags = soFlags;
    fvinfo.statefulObjectNode = soObj;

    parents = {{fv, new EdgeUnconditional()}};
    auto oldstate = state;
    auto prev_cur_v = cur_v;
    cur_v = fv;
    state = READ_ONLY;
    for (auto idx : indices) visit(idx, "index", 1);

    if (dataVals.size() > 0) {
        // Whether the node reads or writes data
        cstring svNamePrefix = soFlagsToString(soFlags);
        auto sv = add_and_connect_vertex(svNamePrefix + " "_cs + name,
                flags | VertexFlags::SO_DATA, node);
        auto &svinfo = (*g)[sv];
        svinfo.soFlags = soFlags;
        svinfo.statefulObjectNode = soObj;

        cur_v = sv;
        // Whether the data is read (node writes) or written (node reads)
        if (hasSOFlag(soFlags, SOFlags::UPDATE) || hasSOFlag(soFlags, SOFlags::CREATE)) {
            state = READ_ONLY;
            for (auto data : dataVals) {
                if (auto *mem = data->to<IR::Member>()) {
                    add_variables(mem, getContext(), true);
                } else {
                    visit(data, "data", 1);
                }
            }
        } else if (hasSOFlag(soFlags, SOFlags::READ)) {
            state = WRITE_ONLY;
            for (auto data : dataVals) {
                if (auto *mem = data->to<IR::Member>()) {
                    add_variables(mem, getContext(), false);
                } else {
                    visit(data, "data", 2);
                }
            }
        }
        parents = {{sv, new EdgeUnconditional()}};
    }
    cur_v = prev_cur_v;
    state = oldstate;
}

void ControlGraphs::visit_call(const cstring &name, const IR::Node *node,
                               VertexFlags flags, std::vector<const IR::Node *> args,
                               std::vector<const IR::Node *> retArgs,
                               const IR::Node *soObj,
                               const IR::Node *callSite) {
    // before visit
    auto call_v = add_and_connect_vertex("CALL "_cs + name,
            VertexFlags::CALL | flags, callSite);
    parents = {{call_v, new EdgeProcedural()}};

    if (args.size() > 0) {
        auto prev_cur_v = cur_v;
        cur_v = call_v;
        auto oldstate = state;
        state = READ_ONLY;
        for (auto *arg : args) {
            if (auto *pe = arg->to<IR::PathExpression>())
                add_variables(pe, getContext(), true);
            else
                add_variable_in_vertex(arg, call_v, true);
        }
        state = oldstate;
        cur_v = prev_cur_v;
    }

    auto pp = getProcedure(node);
    if (pp == emptyProcedure) {
        // Visit
        auto oldProcName = procName;
        procName = name;
        visit(node);
        procName = oldProcName;
        pp = getProcedure(node);
    } else {
        add_and_connect_vertex(pp.first, EdgeType::INTER_PROCEDURE);
        parents = {{pp.second, new EdgeProcedural()}};
    }

    // Maintain callers
    procCallerMaps[graphName][name].push_back(call_v);

    // after visit
    auto ret_v = add_and_connect_vertex("RETURN "_cs + name, VertexFlags::RETURN | flags);

    auto &procEntryInfo = (*g)[pp.first];
    if (procEntryInfo.statefulObjectNode != nullptr) {
        (*g)[call_v].statefulObjectNode = procEntryInfo.statefulObjectNode;
        (*g)[ret_v].statefulObjectNode = procEntryInfo.statefulObjectNode;
    }

    // store callMap (ENTRY, RETURN)
    callMaps[graphName][call_v] = {pp.first, ret_v};

    parents = {{ret_v, new EdgeUnconditional()}};
    add_edge(call_v, ret_v, cstring::empty, EdgeType::CALL_TO_RETURN);

    auto prev_cur_v = cur_v;
    cur_v = ret_v;
    for (size_t i = 0; i < retArgs.size(); i++) {
        const IR::Node *retArg;
        if (auto *pe = retArgs[i]->to<IR::PathExpression>())
            add_variables(pe, getContext(), false, &retArg);
        else
            retArg = add_variable_in_vertex(retArgs[i], ret_v, false);

        BUG_CHECK(i < pp.retVals.size(),
                "Number of proc return values (%1%) are less than number of caller's retArgs (%2%)",
                pp.retVals.size(), i);
        // Store <e_p, retVal> -> <ret_v, retArg>
        retArgEdges[graphName].push_back({{pp.second, pp.retVals[i]},
                {ret_v, retArg}});
    }
    cur_v = prev_cur_v;
}

static const IR::Expression *get_primary(const IR::Expression *e, const Visitor::Context *ctxt) {
    if (ctxt && (ctxt->node->is<IR::Member>() || ctxt->node->is<IR::AbstractSlice>() ||
                 ctxt->node->is<IR::ArrayIndex>())) {
        return get_primary(ctxt->node->to<IR::Expression>(), ctxt->parent);
    } else {
        return e;
    }
}

static const IR::Expression *isValid(const IR::Member *m, const Visitor::Context *ctxt) {
    if (m->member.name == "$valid") return m;
    if (!ctxt || !ctxt->node->is<IR::MethodCallExpression>()) return nullptr;
    if (m->member.name == "isValid" || m->member.name == "setValid" ||
        m->member.name == "setInvalid")
        return ctxt->node->to<IR::Expression>();
    return nullptr;
}

const IR::Expression *ControlGraphs::add_variables(const IR::Expression *e, const Context *ctxt,
        bool isUsed, const IR::Node **addVar) {
    if (!ctxt) {
    } else if (auto *m = ctxt->node->to<IR::Member>()) {
        if (auto *t = isValid(m, ctxt->parent)) {
            add_variable_in_vertex(t, cur_v.value(), isUsed);
            return t;

        } else if (m->expr->type->to<IR::Type_StructLike>()) {
            e = get_primary(m, ctxt->parent);

        } else if (m->expr->type->to<IR::Type_Array>()) {
            if (m->member.name == "lastIndex") {
                e = m;
            } else if (m->member.name == "next" || m->member.name == "last") {
                add_variable_in_vertex(m, cur_v.value(), isUsed);
                e = m;
            } else {
                BUG("invalid read of header stack: %s", m);
            }
        } else {
            BUG("%s: Member of unexpected type %s", m, m->expr->type);
        }
    } else if (auto *sl = ctxt->node->to<IR::Slice>()) {
        // TODO: should I check overlapped slices?
        e = add_variables(sl, ctxt->parent, isUsed, addVar);
        BUG_CHECK(e == sl, "slice %s is not primary in ControlGraphs::add_variables", sl);
        e = sl;
    } else if (auto *ai = ctxt->node->to<IR::ArrayIndex>()) {
        e = get_primary(ai, ctxt->parent);
    }

    const IR::Node *newVar = nullptr;
    if (auto *pe = e->to<IR::PathExpression>()) {
        auto *decl = refMap->getDeclaration(pe->path, false);
        if (decl != nullptr) {
            // Find the declared variable
            if (auto *param = decl->to<IR::Parameter>()) {
                newVar = add_variable_in_vertex(param, cur_v.value(), isUsed);
            } else if (auto *dv = decl->to<IR::Declaration_Variable>()) {
                // Check if global variable
                auto &gvarset = graphVars[graphName];
                bool isGlobal = std::any_of(gvarset.begin(), gvarset.end(),
                        [dv](const IR::Node *v) { return v->equiv(*dv); });
                newVar = isGlobal
                    ? add_variable_in_vertex(dv, cur_v.value(), isUsed)
                    : add_local_variable_in_vertex(dv, cur_v.value(), isUsed);
            }
        }
    } else if (auto *me = e->to<IR::Member>()) {
        // Check if it's egress_port ("standard_metadata.egress_port")
        if (me->expr->is<IR::PathExpression>()) {
            auto *decl = refMap->getDeclaration(me->expr->to<IR::PathExpression>()->path, false);
            if (decl != nullptr && decl->is<IR::Parameter>()) {
                auto *param = decl->to<IR::Parameter>();
                // PNA and PSA set the output port by extern send_to_port()
                if (arch == "v1model") {
                    // Check if the parameter's type is standard_metadata_t
                    if (param->type->is<IR::Type_Name>() &&
                        param->type->to<IR::Type_Name>()->path->name == "standard_metadata_t") {
                        // Check if it's egress_port or egress_spec
                        if (param->direction == IR::Direction::InOut) {
                            if (me->member.name == "egress_spec" || me->member.name == "egress_port") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                egressPortVars[graphName] = newVar;
                            } else if (me->member.name == "ingress_port") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                ingressPortVars[graphName] = newVar;
                            }
                        }
                    }
                } else if (arch == "tna") {
                    // Check if the parameter's type is ingress_intrinsic_metadata_for_tm_t
                    if (param->type->is<IR::Type_Name>()) {
                        auto typeName = param->type->to<IR::Type_Name>();
                        if (typeName->path->name == "ingress_intrinsic_metadata_for_tm_t" &&
                            param->direction == IR::Direction::InOut &&
                            me->member.name == "ucast_egress_port") {
                            newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                            egressPortVars[graphName] = newVar;
                        } else if (typeName->path->name == "egress_intrinsic_metadata_t" &&
                                   param->direction == IR::Direction::In &&
                                   me->member.name == "egress_port") {
                            newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                            egressPortVars[graphName] = newVar;
                        } else if (typeName->path->name == "ingress_intrinsic_metadata_for_tm_t" &&
                                   param->direction == IR::Direction::In &&
                                   me->member.name == "ingress_port") {
                            // XXX
                            newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                            dropVars[graphName] = newVar;
                        } else if (typeName->path->name == "ingress_intrinsic_metadata_t" &&
                                   param->direction == IR::Direction::In &&
                                   me->member.name == "ingress_port") {
                            newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                            ingressPortVars[graphName] = newVar;
                        }
                    }
                } else if (arch == "psa") {
                    // Check if the parameter's type is standard_metadata_t
                    if (param->type->is<IR::Type_Name>()) {
                        auto typeName = param->type->to<IR::Type_Name>();
                        if (typeName->path->name == "psa_ingress_output_metadata_t") {
                            if (param->direction == IR::Direction::InOut) {
                                if (me->member.name == "drop") {
                                    newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                    dropVars[graphName] = newVar;
                                } else if (me->member.name == "egress_port") {
                                    newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                    egressPortVars[graphName] = newVar;
                                }
                            }
                        } else if (typeName->path->name == "psa_egress_output_metadata_t") {
                            if (param->direction == IR::Direction::InOut &&
                                me->member.name == "drop") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                dropVars[graphName] = newVar;
                            }
                        } else if (typeName->path->name == "psa_egress_input_metadata_t") {
                            if (param->direction == IR::Direction::In &&
                                me->member.name == "egress_port") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                egressPortVars[graphName] = newVar;
                            }
                        } else if (typeName->path->name == "psa_ingress_input_metadata_t") {
                            if (param->direction == IR::Direction::In &&
                                me->member.name == "ingress_port") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                ingressPortVars[graphName] = newVar;
                            }
                        }
                    }
                } else if (arch == "pna") {
                    if (param->type->is<IR::Type_Name>()) {
                        auto typeName = param->type->to<IR::Type_Name>();
                        if (typeName->path->name == "pna_main_input_metadata_t" &&
                            param->direction == IR::Direction::In) {
                            if (me->member.name == "input_port") {
                                newVar = add_variable_in_vertex(me, cur_v.value(), isUsed);
                                ingressPortVars[graphName] = newVar;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!newVar)
        add_variable_in_vertex(e, cur_v.value(), isUsed);
    if (addVar)
        *addVar = newVar;
    return e;
}

}  // namespace P4::P4StateDependency
