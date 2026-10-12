#include "backends/chain_extractor/chain_pass.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <queue>

#include "backends/chain_extractor/dependency_graph.h"
#include "backends/ifds/utils.h"
#include "frontends/p4/methodInstance.h"
#include "lib/timer.h"

namespace P4::P4ChainExtractor {

class FindChains::ActDomain : public IDEDomain {
 public:
    cstring name() const override { return "Act"_cs; }
    std::vector<TabVertex> seeds(Tabulation *tab) override { return tab->sgProp->actionParams; }
};

class FindChains::HdrDomain : public IDEDomain {
 public:
    HdrDomain(FindChains &pass, bool withImplicitFlow)
        : pass(pass), withImplicitFlow(withImplicitFlow) {}
    cstring name() const override { return "Hdr"_cs; }
    std::vector<TabVertex> seeds(Tabulation *tab) override { return pass.header_seeds(tab); }
    void add_flow_functions(Tabulation *tab, const std::vector<TabVertex> &seeds,
                            FlowOverlay &overlay) override {
        // <0, 0> -> <CFGroot, hdr>: the header values entering the control
        for (const auto &seed : seeds)
            overlay.add_flow(tab->g, tab->sgProp->rootVar, tab->get_vertex_id(seed),
                             EdgeType::IFDS_FT);
        if (withImplicitFlow) pass.add_implicit_table_flow(tab, overlay);
    }

 private:
    FindChains &pass;
    bool withImplicitFlow;
};

class FindChains::SODomain : public IDEDomain {
 public:
    explicit SODomain(FindChains &pass) : pass(pass) {}
    cstring name() const override { return "SO"_cs; }
    std::vector<TabVertex> seeds(Tabulation *tab) override { return pass.register_read_seeds(tab); }

 private:
    FindChains &pass;
};

namespace {

// Gens the seed's bit on every fact edge into the seed, whatever its source: a definition inside an
// action marks the value as produced by that action.
class GenOnEveryInEdge : public EdgeFuncProvider {
 public:
    std::optional<EdgeFuncHolder> seed_edge_func(Tabulation &tab, Graphs::vertex_t,
                                                 const TabVertex &, size_t seedIdx,
                                                 const EdgeTypeIface &) const override {
        return EdgeFuncHolder(std::make_unique<VarSetFunc>(tab.varBitSetSize, seedIdx));
    }
};

}  // namespace

class FindChains::ActSelDomain : public IDEDomain {
 public:
    explicit ActSelDomain(FindChains &pass) : pass(pass) {}
    cstring name() const override { return "ActSel"_cs; }
    std::vector<TabVertex> seeds(Tabulation *tab) override { return pass.action_def_seeds(tab); }
    const EdgeFuncProvider &edge_funcs() const override {
        static const GenOnEveryInEdge provider;
        return provider;
    }

 private:
    FindChains &pass;
};

namespace {

// Statements where a value reaching the stateful object (as index or data) forms the first hop.
bool is_so_sink(const Graphs::Vertex &vinfo) {
    if (hasFlag(vinfo.flags, VertexFlags::SO_IDX)) {
        // TODO: differentiate stateful CALL and procedures
        // TODO: apply block could be used only for read
        return !hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
               hasSOFlag(vinfo.soFlags, SOFlags::UPDATE) ||
               hasSOFlag(vinfo.soFlags, SOFlags::READ);
    }
    if (hasFlag(vinfo.flags, VertexFlags::SO_DATA)) {
        // Non-stateful SO_DATA vertices are procedure statements
        return !hasFlag(vinfo.flags, VertexFlags::STATEFUL) ||
               hasSOFlag(vinfo.soFlags, SOFlags::UPDATE);
    }
    return false;
}

// Condition sinks: if/switch guards outside RegisterAction bodies. Conditions inside a
// RegisterAction apply body (SO_DATA) are stateful-ALU control flow, not MAU conditions.
bool is_cond_sink(const Graphs::Vertex &vinfo) {
    return (hasFlag(vinfo.flags, VertexFlags::CONDITION) ||
            hasFlag(vinfo.flags, VertexFlags::SWITCH)) &&
           !hasFlag(vinfo.flags, VertexFlags::SO_DATA) && !vinfo.useVars.empty();
}

}  // namespace

bool is_output_lvalue(const IR::Expression *expr, P4::ReferenceMap *refMap) {
    // Output-deciding fields per intrinsic metadata type (v1model and tna)
    static const std::map<cstring, std::set<cstring>> outputs = {
        {"standard_metadata_t"_cs, {"egress_spec"_cs, "mcast_grp"_cs}},
        {"ingress_intrinsic_metadata_for_tm_t"_cs,
         {"ucast_egress_port"_cs, "mcast_grp_a"_cs, "mcast_grp_b"_cs}},
        {"ingress_intrinsic_metadata_for_deparser_t"_cs, {"drop_ctl"_cs}},
        {"egress_intrinsic_metadata_for_deparser_t"_cs, {"drop_ctl"_cs}},
    };
    if (const auto *slice = expr->to<IR::Slice>()) expr = slice->e0;
    const auto *member = expr->to<IR::Member>();
    if (member == nullptr) return false;
    const auto *path = member->expr->to<IR::PathExpression>();
    if (path == nullptr) return false;
    const auto *decl = refMap->getDeclaration(path->path, false);
    const auto *param = decl != nullptr ? decl->to<IR::Parameter>() : nullptr;
    const auto *type = param != nullptr ? param->type->to<IR::Type_Name>() : nullptr;
    if (type == nullptr) return false;
    auto it = outputs.find(type->path->name.name);
    return it != outputs.end() && it->second.count(member->member.name) > 0;
}

bool OutputWriteFinder::writes(const IR::Node *node) {
    found = false;
    node->apply(*this);
    return found;
}

bool OutputWriteFinder::table_writes(const IR::P4Table *table) {
    const auto *actions = table->getActionList();
    if (actions == nullptr) return false;
    for (const auto *el : actions->actionList) {
        const auto *mce = el->expression->to<IR::MethodCallExpression>();
        if (mce == nullptr) continue;
        const auto *mi = P4::MethodInstance::resolve(mce, refMap, typeMap);
        if (const auto *ac = mi->to<P4::ActionCall>())
            if (writes(ac->action->body)) return true;
    }
    return false;
}

bool OutputWriteFinder::preorder(const IR::AssignmentStatement *assign) {
    if (is_output_lvalue(assign->left, refMap)) found = true;
    return !found;
}

bool OutputWriteFinder::preorder(const IR::MethodCallExpression *call) {
    if (found) return false;
    const auto *mi = P4::MethodInstance::resolve(call, refMap, typeMap);
    if (const auto *ef = mi->to<P4::ExternFunction>()) {
        if (ef->method->name.name == "mark_to_drop") found = true;
    } else if (const auto *ac = mi->to<P4::ActionCall>()) {
        visit(ac->action->body);
    } else if (const auto *am = mi->to<P4::ApplyMethod>()) {
        if (const auto *table = am->object->to<IR::P4Table>()) {
            if (const auto *actions = table->getActionList()) {
                for (const auto *el : actions->actionList) {
                    const auto *mce = el->expression->to<IR::MethodCallExpression>();
                    if (mce == nullptr) continue;
                    const auto *act = P4::MethodInstance::resolve(mce, refMap, typeMap);
                    if (const auto *actCall = act->to<P4::ActionCall>())
                        visit(actCall->action->body);
                    if (found) break;
                }
            }
        }
    }
    return !found;
}

bool FindChains::guards_output(const Graphs::Vertex &vinfo) {
    auto it = guardsOutput.find(vinfo.node);
    if (it != guardsOutput.end()) return it->second;
    OutputWriteFinder finder(refMap, typeMap);
    bool writes = false;
    if (const auto *ifs = vinfo.node->to<IR::IfStatement>()) {
        writes = finder.writes(ifs->ifTrue) ||
                 (ifs->ifFalse != nullptr && finder.writes(ifs->ifFalse));
    } else if (const auto *sw = vinfo.node->to<IR::SwitchStatement>()) {
        for (const auto *c : sw->cases)
            if (c->statement != nullptr && finder.writes(c->statement)) {
                writes = true;
                break;
            }
    }
    guardsOutput[vinfo.node] = writes;
    return writes;
}

bool FindChains::key_decides_output(Graph *g, Graphs::vertex_t keyVertex) {
    for (auto [ei, ee] = boost::in_edges(keyVertex, *g); ei != ee; ++ei) {
        auto src = boost::source(*ei, *g);
        if (!hasFlag((*g)[src].flags, VertexFlags::TABLE)) continue;
        const auto *table = (*g)[src].node->to<IR::P4Table>();
        if (table == nullptr) return false;
        auto it = tableWritesOutput.find(table);
        if (it != tableWritesOutput.end()) return it->second;
        OutputWriteFinder finder(refMap, typeMap);
        return tableWritesOutput[table] = finder.table_writes(table);
    }
    return false;
}

const GraphChainMaps &FindChains::maps(cstring graphName) const {
    static const GraphChainMaps empty;
    auto it = graphMaps.find(graphName);
    return it == graphMaps.end() ? empty : it->second;
}

const GraphChainMaps &FindChains::maps(const Graph *graph) const {
    return maps(cstring(boost::get_property(*graph, boost::graph_name)));
}

// Header variables (plus parser-derived metadata and the ingress port) at the CFG root.
std::vector<TabVertex> FindChains::header_seeds(Tabulation *tab) {
    auto *sgProp = tab->sgProp;
    auto *g = tab->g;
    auto rootCFGVit = get_root_vertex(g);
    std::vector<TabVertex> seeds;
    for (size_t i = 1; i < sgProp->progVarInfo.get_var_num(); i++) {
        auto varVit = sgProp->progVarInfo[rootCFGVit][i];
        auto tabTv = tab->get_tab_vertex(varVit);
        auto varInfo = (*g)[varVit];
        if (varInfo.name.startsWith(sgProp->headerVarName) ||
                sgProp->parserMetaSources.count(varInfo.name) ||
                (sgProp->ingressPortVar && sgProp->ingressPortVar->equiv(*tabTv.var))) {
            seeds.push_back(tabTv);
        }
    }
    return seeds;
}

// Every register read of the graph, plus every value used at a stateful-data statement. The
// legacy S2* passes seed the latter when an A2S/H2S edge ends there, so seeding them all lets
// one solve serve both families; collect_so_sinks keeps each family's seeds only.
std::vector<TabVertex> FindChains::register_read_seeds(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    hvec_set<TabVertex, TabVertexHash> seen;
    std::vector<TabVertex> seeds;
    for (const auto &tv : collect_state_vars(g, sgProp, refMap, typeMap)) add_seed(tv, seen, seeds);
    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (!hasFlag(vinfo.flags, VertexFlags::SO_DATA)) continue;
        for (const auto *use : vinfo.useVars) {
            add_seed(TabVertex{*vit, use}, seen, seeds);
            for (const auto *mem : get_var_members(tab, use))
                add_seed(TabVertex{*vit, mem}, seen, seeds);
        }
    }
    return seeds;
}

void FindChains::add_seed(const TabVertex &tv, hvec_set<TabVertex, TabVertexHash> &seen,
                          std::vector<TabVertex> &seeds) {
    if (seen.insert(tv).second) seeds.push_back(tv);
}

std::vector<TabVertex> FindChains::action_def_seeds(Tabulation *tab) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    std::vector<TabVertex> seeds;
    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;
        auto procName = sgProp->procOf[*vit];
        auto srcIt = sgProp->srcOf.find(procName);
        if (srcIt == sgProp->srcOf.end() ||
                !hasFlag((*g)[srcIt->second].flags, VertexFlags::ACTION))
            continue;
        // INPUT action vars are control-plane parameters, not effects of the action
        if (vinfo.name.startsWith("INPUT: ")) continue;
        for (const auto *defVar : vinfo.defVars) {
            auto idx = sgProp->progVarInfo.get_var_index(defVar, procName);
            if (!sgProp->progVarInfo.is_local(idx)) seeds.push_back(TabVertex{*vit, defVar});
        }
        if (hasFlag(vinfo.flags, VertexFlags::DROP) && sgProp->egressPortVar)
            seeds.push_back(TabVertex{*vit, sgProp->egressPortVar});
    }
    return seeds;
}

const std::pair<std::vector<cstring>, cstring> &FindChains::cached_action_of(
        Tabulation *tab, const hvec_map<cstring, cstring> &tableName,
        hvec_map<cstring, std::pair<std::vector<cstring>, cstring>> &cache, const TabVertex &seed) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto procName = sgProp->procOf[seed.node];
    auto it = cache.find(procName);
    if (it != cache.end()) return it->second;
    auto src = sgProp->srcOf[procName];
    const auto *action = (*g)[src].node != nullptr ? (*g)[src].node->to<IR::P4Action>() : nullptr;
    std::pair<std::vector<cstring>, cstring> entry{
        {}, action != nullptr ? cstring(action->controlPlaneName()) : procName};
    for (const auto &caller : get_tables_from_action(tab, src)) {
        auto t = tableName.find(caller);
        if (t != tableName.end()) entry.first.push_back(t->second);
    }
    return cache.emplace(procName, entry).first->second;
}

void FindChains::collect_hops(Tabulation *tab, const DomainSet &domains, size_t so,
                              size_t actSel, GraphChainMaps &m) {
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;

    // Tables: control-plane name, CFG order, register reads reaching the key, output actions
    hvec_map<cstring, cstring> tableName;   // CFG procedure name -> control-plane name
    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (!hasFlag(vinfo.flags, VertexFlags::TABLE) || !hasFlag(vinfo.flags, VertexFlags::ENTRY))
            continue;
        const auto *table = vinfo.node != nullptr ? vinfo.node->to<IR::P4Table>() : nullptr;
        if (table == nullptr) continue;
        auto name = table->controlPlaneName();
        tableName[sgProp->procOf[*vit]] = name;
        m.tableOrder.emplace(name, m.tableOrder.size());
        auto key = get_table_key(tab, *vit);
        if (!key.has_value()) continue;
        for (const auto *use : (*g)[*key].useVars)
            for (const auto &seed : domains.targets(*tab, tab->valueMap[TabVertex{*key, use}], so))
                if (const auto *soNode = (*g)[seed.node].statefulObjectNode)
                    m.keyReads[name].insert(DependencyGraphs::so_name(soNode));
        if (!key_decides_output(g, *key)) continue;
        OutputWriteFinder finder(refMap, typeMap);
        for (const auto *el : table->getActionList()->actionList) {
            const auto *mce = el->expression->to<IR::MethodCallExpression>();
            if (mce == nullptr) continue;
            const auto *ac = P4::MethodInstance::resolve(mce, refMap, typeMap)->to<P4::ActionCall>();
            if (ac != nullptr && finder.writes(ac->action->body))
                m.outputActions[name].push_back(ac->action->controlPlaneName());
        }
    }

    // ActSel seed -> (caller tables, action), cached per action procedure
    hvec_map<cstring, std::pair<std::vector<cstring>, cstring>> actionOf;

    // Output sinks: the actions whose effect reaches each used fact, grouped by table
    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        bool outKey = hasFlag(vinfo.flags, VertexFlags::KEY) && key_decides_output(g, *vit);
        bool outCond = is_cond_sink(vinfo) && guards_output(vinfo);
        if (!outKey && !outCond) continue;
        for (const auto *use : vinfo.useVars) {
            auto &select = m.sinkSelect[{*vit, use}];
            for (const auto &seed : domains.targets(*tab, tab->valueMap[TabVertex{*vit, use}], actSel)) {
                const auto &[tables, action] = cached_action_of(tab, tableName, actionOf, seed);
                for (const auto &t : tables) select[t].insert(action);
            }
        }
    }
}

void FindChains::add_implicit_table_flow(Tabulation *tab, FlowOverlay &overlay) {
    auto *sgProp = tab->sgProp;
    g = tab->g;

    // 1. Map from tableName to its key variable indices
    hvec_map<cstring, std::vector<size_t>> tableKeyVars;
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (!hasFlag(vinfo.flags, VertexFlags::TABLE) ||
                !hasFlag(vinfo.flags, VertexFlags::ENTRY))
            continue;

        auto tableName = sgProp->procOf[*vit];
        auto keyOpt = get_table_key(tab, *vit);
        if (!keyOpt.has_value()) continue;
        auto keyInfo = (*g)[keyOpt.value()];
        for (auto kv : keyInfo.useVars) {
            auto varIdx = sgProp->progVarInfo.get_var_index(kv, tableName);
            // Skip local variables since they cannot be propagated to callee
            if (sgProp->progVarInfo.is_local(varIdx)) continue;
            tableKeyVars[tableName].push_back(varIdx);
        }
    }

    // 2. Edges from the match keys to the defVars of every action of the table
    std::size_t n = num_vertices(*g);
    auto index = boost::get(boost::vertex_index, *g);
    std::vector<bool> visited(n, false);
    std::queue<Graphs::vertex_t> q;

    q.push(get_root_vertex(g));
    while (!q.empty()) {
        auto u = q.front();
        q.pop();

        for (auto [ei, ei_end] = boost::out_edges(u, *g); ei != ei_end; ++ei) {
            auto &edge = (*g)[*ei];
            if (edge.type == EdgeType::HAS_VAR) continue;
            if (edge.type == EdgeType::IFDS) continue;
            if (edge.type == EdgeType::IFDS_FT) continue;
            auto v = boost::target(*ei, *g);
            auto vid = index[v];
            auto vinfo = (*g)[v];
            auto procName = sgProp->procOf[v];
            auto procSrcit = sgProp->srcOf[procName];
            auto procSrcInfo = (*g)[procSrcit];
            // Only action statements; INPUT action vars are defined from the control plane
            // (i.e. <u, 0> -> <v, defVar>)
            if (hasFlag(procSrcInfo.flags, VertexFlags::ACTION) &&
                    !vinfo.name.startsWith("INPUT: ")) {
                for (auto defVar : vinfo.defVars) {
                    auto defVarId = sgProp->progVarInfo.get_var_index(defVar, procName);
                    auto defVarVit = sgProp->progVarInfo[v][defVarId];
                    if (sgProp->progVarInfo.is_local(defVarId)) continue;

                    // Create IFDS edge from <u, kv> to <v, defVar> for each caller table
                    for (auto tableName : get_tables_from_action(tab, procSrcit)) {
                        for (auto keyVarIdx : tableKeyVars[tableName]) {
                            if (keyVarIdx == defVarId) continue;
                            auto keyVit = sgProp->progVarInfo[u][keyVarIdx];
                            BUG_CHECK(keyVit != 0,
                                      "Key variable %1%(%2%) is out of bounds for table %3%",
                                      sgProp->progVarInfo.get_var(keyVarIdx, tableName),
                                      keyVarIdx, tableName);
                            overlay.add_flow(tab->g, keyVit, defVarVit, EdgeType::IFDS);
                        }
                    }
                }
            }

            if (!visited[vid]) {
                visited[vid] = true;
                q.push(v);
            }
        }
    }
}

std::vector<TabVertex> FindChains::get_seeds(Tabulation *tab) {
    ActDomain act;
    HdrDomain hdr(*this, false);
    SODomain so(*this);
    DomainSet domains({&act, &hdr, &so});
    domains.layout(tab);
    return domains.seeds();
}

void FindChains::collect_so_sinks(Tabulation *tab, const DomainSet &domains, size_t so,
                                  const std::vector<TabVertex> &familySeeds,
                                  const SinkMaps &sinks) {
    if (familySeeds.empty()) return;
    auto *g = tab->g;
    auto *sgProp = tab->sgProp;
    auto mainProcName = sgProp->procOf[sgProp->rootVar];

    // The family's register reads present in a value, in the family's seed order (the bit
    // order a solve seeded with exactly these reads would report).
    hvec_map<TabVertex, size_t, TabVertexHash> rank;
    for (size_t i = 0; i < familySeeds.size(); i++) rank.emplace(familySeeds[i], i);
    TargetsFn targets = [&](const VarBitSet &value) {
        std::vector<std::pair<size_t, TabVertex>> ranked;
        for (const auto &tv : domains.targets(*tab, value, so)) {
            auto it = rank.find(tv);
            if (it != rank.end()) ranked.emplace_back(it->second, tv);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<TabVertex> out;
        for (auto &[_, tv] : ranked) out.push_back(tv);
        return out;
    };
    auto into = [](IDEPass::DepEdgeMap *m) { return [m]() -> IDEPass::DepEdgeMap & { return *m; }; };
    auto hdrSink = [sgProp](const TabVertex &varTv, const Graphs::Vertex &varInfo) {
        return varInfo.name.startsWith(sgProp->headerVarName) ||
               sgProp->parserMetaSources.count(varInfo.name) ||
               (sgProp->egressPortVar && sgProp->egressPortVar->equiv(*varTv.var));
    };

    auto outSink = [this](const TabVertex &varTv, const Graphs::Vertex &) {
        const auto *expr = varTv.var->to<IR::Expression>();
        return expr != nullptr && is_output_lvalue(expr, refMap);
    };

    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::KEY)) {
            if (sinks.key) collect_dep_edges_impl(tab, *vit, true, targets, into(sinks.key));
            if (sinks.keyHdr) collect_dep_edges_impl(tab, *vit, true, targets, into(sinks.keyHdr));
            if (sinks.outKey && key_decides_output(g, *vit))
                collect_dep_edges_impl(tab, *vit, true, targets, into(sinks.outKey));
        }
        if (hasFlag(vinfo.flags, VertexFlags::EXIT) && sgProp->procOf[*vit] == mainProcName) {
            if (sinks.hdr) collect_dep_edges_to_impl(tab, *vit, hdrSink, targets, into(sinks.hdr));
            if (sinks.keyHdr)
                collect_dep_edges_to_impl(tab, *vit, hdrSink, targets, into(sinks.keyHdr));
            if (sinks.out) collect_dep_edges_to_impl(tab, *vit, outSink, targets, into(sinks.out));
        }
        if (is_cond_sink(vinfo)) {
            if (sinks.cond) collect_dep_edges_impl(tab, *vit, true, targets, into(sinks.cond));
            if (sinks.outCond && guards_output(vinfo))
                collect_dep_edges_impl(tab, *vit, true, targets, into(sinks.outCond));
        }
    }
}

void FindChains::print_counters(Tabulation *tab) {
    auto *g = tab->g;
    auto graphName = boost::get_property(*g, boost::graph_name);
    std::array<size_t, 6> numEntities{};
    for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
        const auto &vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) {
            if (vinfo.interesting) numEntities[2]++;   // reachable ESG nodes
            numEntities[1]++;                          // ESG nodes
        } else {
            numEntities[0]++;                          // nodes
        }
    }
    for (auto [eit, eend] = boost::edges(*g); eit != eend; ++eit) {
        const auto &edge = (*g)[*eit];
        if (edge.type == EdgeType::IFDS || edge.type == EdgeType::IFDS_FT) {
            const auto &srcInfo = (*g)[boost::source(*eit, *g)];
            const auto &dstInfo = (*g)[boost::target(*eit, *g)];
            if (srcInfo.interesting && dstInfo.interesting) numEntities[5]++;
            numEntities[4]++;
        } else if (edge.type != EdgeType::HAS_VAR) {
            numEntities[3]++;
        }
    }
    std::cout << "================" << std::endl;
    std::cout << "[RESULT] Chains in " << graphName << ":\n";
    std::cout << "Total nodes: " << numEntities[0] << std::endl;
    std::cout << "Total edges: " << numEntities[3] << std::endl;
    std::cout << "Total ESG nodes: " << numEntities[1] << std::endl;
    std::cout << "Total ESG edges: " << numEntities[4] << std::endl;
    std::cout << "Reachable ESG nodes: " << numEntities[2] << std::endl;
    std::cout << "Reachable ESG edges: " << numEntities[5] << std::endl;
    std::cout << "================" << std::endl << std::endl;
}

void FindChains::analyze_control_graph(Tabulation *tab) {
    g = tab->g;
    auto *sgProp = tab->sgProp;
    auto graphName = cstring(boost::get_property(*g, boost::graph_name));
    if (!needAct && !needHdr) return;

    ActDomain act;
    HdrDomain hdr(*this, legacyFlow);
    SODomain so(*this);
    ActSelDomain actSel(*this);

    // Default: one solve {Act, Hdr, SO}; the implicit table flow carries every domain.
    // Legacy: {Act, SO} without implicit flow, and {Hdr} with it (as p4c_state_dependency).
    DomainSet domains;
    DomainSet hdrOnly({&hdr});
    if (needAct) domains.add(&act);
    if (needHdr && !legacyFlow) domains.add(&hdr);
    domains.add(&so);
    if (needHops) domains.add(&actSel);
    Tabulation hdrTab(g, sgProp);
    Tabulation *tabH = legacyFlow ? &hdrTab : tab;
    const DomainSet &domainsH = legacyFlow ? hdrOnly : domains;
    bool solved = false;
    bool solvedH = false;
    {
        Util::ScopedTimer solveTimer("Solve");
        if (!legacyFlow) add_implicit_table_flow(tab, flowOverlay);
        solved = solve(tab, domains);
        if (legacyFlow && needHdr) solvedH = solve(&hdrTab, hdrOnly);
        else solvedH = solved;
    }

    {
        Util::ScopedTimer collectTimer("Collect");
        auto &m = graphMaps[graphName];
        auto actIdx = domains.index_of("Act"_cs);
        auto soIdx = domains.index_of("SO"_cs);
        auto hdrIdx = domainsH.index_of("Hdr"_cs);

        // First hop: values reaching a stateful object (A2S: src -> dst; H2S: dst -> src)
        for (auto [vit, vend] = boost::vertices(*g); vit != vend; ++vit) {
            if (!is_so_sink((*g)[*vit])) continue;
            if (solved && needAct && actIdx)
                collect_dep_edges(tab, *vit, domains, *actIdx, m.a2s, true);
            if (solvedH && needHdr && hdrIdx)
                collect_dep_edges(tabH, *vit, domainsH, *hdrIdx, m.h2s, false);
        }
        m.aStateVars = collect_state_vars_from_dep_edges_dst(g, sgProp, refMap, typeMap, m.a2s,
                                                             m.aStateVarMap, true);
        m.hStateVars = collect_state_vars_from_dep_edges_src(g, sgProp, refMap, typeMap, m.h2s,
                                                             m.hStateVarMap, true);

        // Second hop: the family's register reads reaching key / header-port / condition sinks
        if (solved && soIdx) {
            if (needAct) {
                SinkMaps a;
                a.key = &m.a2s2k;
                a.keyHdr = &m.a2s2kv;
                a.cond = &m.a2s2c;
                a.out = &m.a2s2o;
                a.outCond = &m.a2s2outc;
                a.outKey = &m.a2s2outk;
                collect_so_sinks(tab, domains, *soIdx, m.aStateVars, a);
            }
            if (needHdr) {
                SinkMaps h;
                h.key = &m.h2s2k;
                h.hdr = &m.h2s2v;
                h.cond = &m.h2s2c;
                h.out = &m.h2s2o;
                h.outCond = &m.h2s2outc;
                h.outKey = &m.h2s2outk;
                collect_so_sinks(tab, domains, *soIdx, m.hStateVars, h);
            }
            if (auto actSelIdx = domains.index_of("ActSel"_cs))
                collect_hops(tab, domains, *soIdx, *actSelIdx, m);
        }
    }

    print_counters(tab);
    flowOverlay.restore();
    tab->clear_edge_func();
    if (legacyFlow) hdrTab.clear_edge_func();
}

Visitor::profile_t FindChains::init_apply(const IR::Node *n) {
    graphMaps.clear();
    for (size_t i = 0; i < controlGraphsArray->size(); i++) {
        Tabulation tab((*controlGraphsArray)[i], (*graphProps)[i]);
        analyze_control_graph(&tab);
    }
    return Inspector::init_apply(n);
}

}  // namespace P4::P4ChainExtractor
