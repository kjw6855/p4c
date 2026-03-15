#include "tabulation.h"
#include "graphs.h"

namespace P4::P4StateDependency {

bool Tabulation::init_edge_func(const std::vector<TabVertex> &from) {
    auto &progVarInfo = sgProp->progVarInfo;
    targetVars = from;
    varBitSetSize = targetVars.size() + 1;  // varBitSetSize >= 1+vars
    targetVarIdMap.clear();
    topFunc = EdgeFuncHolder(std::make_unique<TopFunc>(varBitSetSize));
    topEnvValue = topFunc.getValue().value();

    for (size_t i = 0 ; i < targetVars.size(); i++) {
        auto &tv = targetVars[i];
        auto procName = sgProp->procOf[tv.node];
        auto varIdx = progVarInfo.get_var_index(tv.var, procName);
        auto varIt = progVarInfo[tv.node][varIdx];

        for (auto [ei, ei_end] = boost::in_edges(varIt, *g);
                ei != ei_end; ++ei) {
            auto &edge = (*g)[*ei];
            if (edge.type != EdgeType::IFDS &&
                    edge.type != EdgeType::IFDS_FT) continue;
            auto srcVarIt = boost::source(*ei, *g);
            auto srcTv = get_tab_vertex(srcVarIt);

            if (progVarInfo[srcTv.node][0] == srcVarIt) {
                // FOUND if <n, 0> -> <m, v>
                edge.setFunc(std::make_unique<VarSetFunc>(varBitSetSize, i));
                targetVarIdMap[tv] = i;
                break;      // should be one edge from 0
            }
        }
    }

    return (targetVarIdMap.size() > 0);
}

std::optional<size_t> Tabulation::get_target_var_id(const TabVertex &tv) {
    auto targetVarIdMapIt = targetVarIdMap.find(tv);
    return targetVarIdMapIt == targetVarIdMap.end() ?
        std::nullopt : std::optional{targetVarIdMapIt->second};
}

std::vector<TabVertex> Tabulation::get_target_vars(const VarBitSet &bitmap) {
    if (bitmap.none() || bitmap == topEnvValue) return {};

    std::vector<TabVertex> foundTargetVars;
    // Get first set bit
    size_t pos = bitmap.find_first();
    while (pos != VarBitSet::npos) {
        foundTargetVars.push_back(targetVars[pos]);
        // Get next set bit after current pos
        pos = bitmap.find_next(pos);
    }
    return foundTargetVars;
}

void Tabulation::clear_edge_func() {
    for (auto [ei, ei_end] = boost::edges(*g); ei != ei_end; ++ei) {
        auto &edge = (*g)[*ei];
        if (edge.type != EdgeType::IFDS &&
                edge.type != EdgeType::IFDS_FT) continue;

        edge.fn = globalIdFunc;
    }
}

void Tabulation::init_ifds() {
    // Initialize metadata
    pathEdge.clear();
    workList = std::queue<TabEdge>();
    summaryEdge.clear();

    pathEdge.insert({rootTv, rootTv});
    workList.push({rootTv, rootTv});
}

void Tabulation::init_ide() {
    // Initialize metadata
    workList = std::queue<TabEdge>();
    incomingList.clear();
    endSummary.clear();
    jumpFunc = FuncMapHelper();
    summaryFunc = FuncMapHelper();

    BUG_CHECK(targetVars.size() > 0, "targeVars does not exist.");
    BUG_CHECK(topFunc.getValue().has_value() &&
              (topFunc.getValue().value() == topEnvValue),
              "topFunc is not set");

    auto vertices = boost::vertices(*g);

    // Line 1-2: Init jumpFunc
    vertices = boost::vertices(*g);
    std::optional<Graphs::vertex_t> exit_v{};
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;
        auto vProcName = sgProp->procOf[*vit];
        auto &variableList = sgProp->progVarInfo.get_all_vars(vProcName);
        // 1) main process
        if (vProcName == sgProp->procOf[sgProp->rootVar]) {
            for (auto *p : variableList)
                jumpFunc.add_func({rootTv, TabVertex{*vit, p}}, topFunc);
            if (hasFlag(vinfo.flags, VertexFlags::EXIT))
                exit_v = *vit;
        } else {
            auto src = sgProp->srcOf[vProcName];
            // Special JumpFunc from any src to main exit
            if (src == *vit && exit_v.has_value()) {
                for (auto *p : variableList)
                    for (auto *q : variableList)
                        jumpFunc.add_func({TabVertex{src, p}, TabVertex{exit_v.value(), q}},
                                topFunc);
            }
            for (auto *p : variableList)
                for (auto *q : variableList)
                    jumpFunc.add_func({TabVertex{src, p}, TabVertex{*vit, q}}, topFunc);
        }
    }

    // Line 3-4: Init summaryFunc
    for (auto [ei, ei_end] = boost::edges(*g); ei != ei_end; ++ei) {
        auto &edge = (*g)[*ei];
        if (edge.type == EdgeType::IFDS ||
                edge.type == EdgeType::IFDS_FT) {
            // Line 1-2: Init jumpFunc
            auto srcTv = get_tab_vertex(boost::source(*ei, *g));
            auto dstTv = get_tab_vertex(boost::target(*ei, *g));
            // TODO: check d, d' should be all pair of data facts
            if (sgProp->procOf[srcTv.node] == sgProp->procOf[dstTv.node])
                jumpFunc.add_func({srcTv, dstTv}, topFunc);

        } else if (edge.type == EdgeType::CALL_TO_RETURN) {
            auto src = boost::source(*ei, *g);
            auto dst = boost::target(*ei, *g);

            auto vProcName = sgProp->procOf[src];
            BUG_CHECK(sgProp->procOf[dst] == vProcName,
                    "%1%::Return has different procName rather than %2%",
                    sgProp->procOf[dst], vProcName);

            auto &variableList = sgProp->progVarInfo.get_all_vars(vProcName);
            for (auto *p : variableList)
                for (auto *q : variableList)
                    summaryFunc.add_func({TabVertex{src, p}, TabVertex{dst, q}}, topFunc);
        }
    }

    workList.push({rootTv, rootTv});
    jumpFunc.add_func({rootTv, rootTv}, globalIdFunc);
}

void Tabulation::propagate_ifds(TabVertex a, TabVertex b) {
    LOG5(dump_tab_vertex(a) << "->" << dump_tab_vertex(b));

    TabEdge ab = {a, b};
    if (pathEdge.find(ab) == pathEdge.end()) {
        pathEdge.insert(ab);
        workList.push(ab);
    }
}

void Tabulation::propagate_ide(TabVertex a, TabVertex b, EdgeFuncHolder fn) {

    TabEdge ab = {a, b};
    auto newFn = fn.may_join(jumpFunc[ab]);
    std::stringstream sstream;
    sstream << dump_tab_vertex(a)
        << "->" << dump_tab_vertex(b)
        << ":" << newFn.getName();
    if (newFn.getValue().has_value())
        sstream << "=" << newFn.getValue().value();
    else
        sstream << "=id";
    LOG5(cstring(sstream));

    if (newFn.getValue() != jumpFunc[ab].getValue()) {
        auto avit = get_vertex_id(a);
        auto bvit = get_vertex_id(b);
        auto &avinfo = (*g)[avit];
        auto &bvinfo = (*g)[bvit];
        bvinfo.color = "black"_cs;
        avinfo.interesting = bvinfo.interesting = true;

        jumpFunc[ab] = newFn;
        workList.push(ab);
    }
}

std::vector<TabVertex> &Tabulation::get_successors(TabVertex &tb,
        std::vector<TabVertex> &succ) {
    // Find var vertex
    auto tbVit = get_vertex_id(tb);
    for (auto [ei, ei_end] = boost::out_edges(tbVit, *g); ei != ei_end; ++ei) {
        succ.push_back(get_tab_vertex(boost::target(*ei, *g)));
    }

    return succ;
}

EdgeFuncHolder Tabulation::get_edge_func(TabVertex &a, TabVertex &b) {
    auto tva = get_vertex_id(a);
    auto tvb = get_vertex_id(b);
    for (auto [ei, ei_end] = boost::out_edges(tva, *g); ei != ei_end; ++ei) {
        if (boost::target(*ei, *g) == tvb) {
            auto edge = (*g)[*ei];
            return edge.fn;
        }
    }
    return globalIdFunc;  // XXX
}

void Tabulation::forward_tabulate_ifds() {
    LOG5("=== (BEGIN) IFDS Tabulate Process ===");
    while (!workList.empty()) {
        TabEdge te = workList.front();
        workList.pop();

        auto srcProc = sgProp->procOf[te.first.node];
        auto dstProc = sgProp->procOf[te.second.node];
        auto dstInfo = (*g)[te.second.node];
        if (hasFlag(dstInfo.flags, VertexFlags::CALL)) {
            auto [calleeEntry, callerRet] = sgProp->get_call_map(te.second.node);
            auto dstVar = get_vertex_id(te.second);
            for (auto [ei, ei_end] = boost::out_edges(dstVar, *g);
                    ei != ei_end; ++ei) {
                auto targetVar = boost::target(*ei, *g);
                auto targetTabTv = get_tab_vertex(targetVar);
                // Line 14-16: propagate into callee start
                if (targetTabTv.node == calleeEntry) {
                    propagate_ifds(targetTabTv, targetTabTv);
                }

                // Line 17-19 (1) short-circuit
                if (targetTabTv.node == callerRet) {
                    propagate_ifds(te.first, targetTabTv);
                }
            }

            // Line 17-19: (2) if summary edge exists, short-circuit
            for (auto se : summaryEdge) {
                if (se.first.node == te.second.node &&
                        se.second.node == callerRet) {
                    propagate_ifds(te.first, se.second);
                }
            }
        } else if (hasFlag(dstInfo.flags, VertexFlags::EXIT) &&
                srcProc == dstProc) {
            auto srcVar = get_vertex_id(te.first);
            auto dstVar = get_vertex_id(te.second);

            // Line 22: For every caller
            for (auto cvit : sgProp->procCallerMap[srcProc]) {

                // Line 23-1: <c, d4> -> srcVar(<s_p, d1>)
                for (auto [ei, ei_end] = boost::in_edges(srcVar, *g);
                        ei != ei_end; ++ei) {
                    auto &edge = (*g)[*ei];
                    // Skip CFG nodes among sources
                    if (edge.type == EdgeType::HAS_VAR) continue;
                    auto sourceVar = boost::source(*ei, *g);
                    auto sourceTabTv = get_tab_vertex(sourceVar);
                    if (sourceTabTv.node != cvit) continue;

                    // Line 23-2: dstVar(<e_p, d2>) -> <ret, d5>
                    for (auto [ej, ej_end] = boost::out_edges(dstVar, *g);
                            ej != ej_end; ++ej) {
                        auto targetVar = boost::target(*ej, *g);
                        auto targetTabTv = get_tab_vertex(targetVar);
                        auto [_, retSite] = sgProp->get_call_map(cvit);
                        if (targetTabTv.node != retSite) continue;
                        // Line 24
                        TabEdge te = {sourceTabTv, targetTabTv};
                        if (summaryEdge.find(te) == summaryEdge.end()) {
                            summaryEdge.insert(te);
                            // Line 26-28
                            auto callProcName = sgProp->procOf[cvit];
                            for (auto pe : pathEdge) {
                                if (pe.second.node == sourceTabTv.node &&
                                        pe.second.var == sourceTabTv.var &&
                                        sgProp->procOf[pe.first.node] == callProcName)
                                    propagate_ifds(pe.first, targetTabTv);
                            }
                        }
                    }
                }
            }
        } else {
            std::vector<TabVertex> succ;
            for (auto tb : get_successors(te.second, succ)) {
                propagate_ifds(te.first, tb);
            }
        }
    }
    LOG5("=== (END) IFDS Tabulate Process ===");

    LOG2("=== Meet-Over-All-Valid-Paths Solutions ===\n");
    for (auto pe : pathEdge) {
        LOG2(dump_tab_vertex(pe.first) << "->" << dump_tab_vertex(pe.second));
        auto fvVit = get_vertex_id(pe.first);
        auto &fvInfo = (*g)[fvVit];
        auto svVit = get_vertex_id(pe.second);
        auto &svInfo = (*g)[svVit];

        svInfo.color = "black"_cs;
        fvInfo.interesting = svInfo.interesting = true;
    }

    LOG2("=== SummaryEdges ===\n");
    for (auto se : summaryEdge) {
        LOG2(dump_tab_vertex(se.first) << "->" << dump_tab_vertex(se.second));
    }

    LOG2("=== X ===");
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;
        auto vProcName = sgProp->procOf[*vit];
        LOG5(vProcName << "  ProcOf " << vinfo.name);
        for (auto pe : pathEdge) {
            auto firstProcName = sgProp->procOf[pe.first.node];
            if (firstProcName == vProcName && pe.second.node == *vit) {
                reachableVars[*vit].push_back(pe.second.var);
            }
        }

        if (reachableVars[*vit].size() > 0) {
            std::stringstream sstream;
            sstream << "  " << vinfo.name << "(" << *vit << "):";
            for (auto rn : reachableVars[*vit]) {
                sstream << " " << rn;
            }
            LOG2(cstring(sstream));
        }
    }
}

void Tabulation::forward_tabulate_ide() {
    LOG5("=== (BEGIN) IDE Tabulate Process ===");
    while (!workList.empty()) {
        TabEdge te = workList.front();
        workList.pop();

        auto fn = jumpFunc[te];
        auto srcProc = sgProp->procOf[te.first.node];
        auto dstProc = sgProp->procOf[te.second.node];
        auto dstInfo = (*g)[te.second.node];
        if (hasFlag(dstInfo.flags, VertexFlags::CALL)) {
            auto [calleeEntry, callerRet] = sgProp->get_call_map(te.second.node);

            auto dstVar = get_vertex_id(te.second);
            for (auto [ei, ei_end] = boost::out_edges(dstVar, *g);
                    ei != ei_end; ++ei) {
                auto targetVar = boost::target(*ei, *g);
                auto targetTabTv = get_tab_vertex(targetVar);
                // Line 12-13: propagate into callee start
                if (targetTabTv.node == calleeEntry) {
                    propagate_ide(targetTabTv, targetTabTv, globalIdFunc);
                }

                // Line 14-16 (1) short-circuit
                if (targetTabTv.node == callerRet) {
                    auto efn = get_edge_func(te.first, targetTabTv);
                    propagate_ide(te.first, targetTabTv, efn.compose(fn));
                }
            }

            // Line 17-18: (2) if summary edge exists, short-circuit
            for (auto sf : summaryFunc.get_func_by_nodes(te.second.node, callerRet)) {
                auto se = sf.first;
                // Find sfn from <n, d2>, not any <n, d>
                if (se.first.var != te.second.var) continue;
                auto sfn = sf.second;
                // skip T fn
                if (sfn.getValue() == topEnvValue)
                    continue;
                // <s_p, d1> -> <ret_n, d3>
                propagate_ide(te.first, se.second, sfn.compose(fn));
            }
        } else if (hasFlag(dstInfo.flags, VertexFlags::EXIT) &&
                srcProc == dstProc) {
            auto srcVar = get_vertex_id(te.first);
            auto dstVar = get_vertex_id(te.second);

            // Line 20: For every caller
            for (auto cvit : sgProp->procCallerMap[srcProc]) {

                // Line 21-1: <c, d4> -> srcVar(<s_p, d1>)
                for (auto [ei, ei_end] = boost::in_edges(srcVar, *g);
                        ei != ei_end; ++ei) {
                    auto &edge = (*g)[*ei];
                    // Skip CFG nodes among sources
                    if (edge.type == EdgeType::HAS_VAR) continue;
                    auto sourceVar = boost::source(*ei, *g);
                    auto sourceTabTv = get_tab_vertex(sourceVar);       // <c, d4>
                    if (sourceTabTv.node != cvit) continue;

                    // Line 21-2: dstVar(<e_p, d2>) -> <ret, d5>
                    for (auto [ej, ej_end] = boost::out_edges(dstVar, *g);
                            ej != ej_end; ++ej) {
                        auto targetVar = boost::target(*ej, *g);
                        auto targetTabTv = get_tab_vertex(targetVar);   // <ret, d5>
                        auto [_, retSite] = sgProp->get_call_map(cvit);
                        if (targetTabTv.node != retSite) continue;

                        TabEdge newTe = {sourceTabTv, targetTabTv};
                        // Line 22-24
                        auto f4 = get_edge_func(sourceTabTv, te.first);
                        auto f5 = get_edge_func(te.second, targetTabTv);
                        auto sfn = summaryFunc[newTe];     // XXX
                        auto fPrime = (f5.compose(fn.compose(f4))).may_join(sfn);

                        // Line 25-29
                        if (fPrime.getValue() != sfn.getValue()) {
                            summaryFunc[newTe] = fPrime;

                            // Line 26-29
                            auto callProcName = sgProp->procOf[cvit];
                            for (auto jf : jumpFunc.get_func_by_second(sourceTabTv)) {
                                auto jfn = jf.second;
                                // skip T fn
                                if (jfn.getValue() == topEnvValue)
                                    continue;
                                auto je = jf.first;
                                if (sgProp->procOf[je.first.node] == callProcName)
                                    propagate_ide(je.first, targetTabTv, fPrime.compose(jfn));
                            }
                        }
                    }
                }
            }
        } else {
            std::vector<TabVertex> succ;
            for (auto tb : get_successors(te.second, succ)) {
                auto efn = get_edge_func(te.second, tb);
                propagate_ide(te.first, tb, efn.compose(fn));
            }
        }
    }
    LOG5("=== (END) IDE Tabulate Process ===");
}

std::vector<TabVertex> Tabulation::get_return_val(const TabVertex &exitTv,
        const TabVertex &callerTv) {
    auto exitVit = get_vertex_id(exitTv);
    auto [_, retSite] = sgProp->get_call_map(callerTv.node);

    std::vector<TabVertex> retVals;
    for (auto [ei, ei_end] = boost::out_edges(exitVit, *g); ei != ei_end; ++ei) {
        auto retVit = boost::target(*ei, *g);
        auto retTabTv = get_tab_vertex(retVit);
        if (retSite != retTabTv.node) continue;

        // TODO: check if it's okay to add d2 for <caller, d2> -> <ret, d5>
        for (auto [ej, ej_end] = boost::in_edges(retVit, *g); ej != ej_end; ++ej) {
            auto &edge = (*g)[*ej];
            if (edge.type == EdgeType::HAS_VAR) continue;

            auto srcVit = boost::source(*ej, *g);
            if (srcVit == exitVit) continue;

            auto srcTabTv = get_tab_vertex(srcVit);
            if (srcTabTv.node != callerTv.node) continue;

            // If source is caller, store found retTabTv
            retVals.push_back(retTabTv);
        }
    }
    return retVals;
}

void Tabulation::forward_tabulate_on_demand_ide() {
    LOG5("=== (BEGIN) IDE Tabulate Process ===");
    while (!workList.empty()) {
        TabEdge te = workList.front();
        workList.pop();

        auto fn = jumpFunc[te];
        auto srcProc = sgProp->procOf[te.first.node];
        auto dstProc = sgProp->procOf[te.second.node];
        auto dstInfo = (*g)[te.second.node];
        if (hasFlag(dstInfo.flags, VertexFlags::CALL)) {
            // <s_q, d1> -> <c, d2>
            // <c> -> <s_p, r>
            auto [calleeEntry, callerRet] = sgProp->get_call_map(te.second.node);

            auto dstVar = get_vertex_id(te.second);
            for (auto [ei, ei_end] = boost::out_edges(dstVar, *g);
                    ei != ei_end; ++ei) {
                auto targetVar = boost::target(*ei, *g);
                auto targetTabTv = get_tab_vertex(targetVar);
                // Line 12-13: propagate into callee start
                if (targetTabTv.node == calleeEntry) {
                    // targetTabTv = <s_p, d1>
                    incomingList[targetTabTv].push_back(te.second);
                    propagate_ide(targetTabTv, targetTabTv, globalIdFunc);
                    // Line 15.2-15.6 (CC10 paper)
                    for (auto es : get_end_summary(targetTabTv)) {
                        for (auto rv : get_return_val(es, te.second)) {
                            // TODO
                            auto exitTabTv = es;    // <e_p, d4>
                            auto retTabTv = rv;     // <r, d5>

                            // <n=c, d2>, <r, d5>
                            TabEdge newTe = {te.second, retTabTv};
                            // <c, d2>, <s_p, d1>
                            auto f4 = get_edge_func(te.second, targetTabTv);
                            // <e_p, d2> -> <r, d5>
                            auto f5 = get_edge_func(exitTabTv, retTabTv);
                            auto sfn = summaryFunc[newTe];     // XXX
                            auto fPrime = (f5.compose(fn.compose(f4))).may_join(sfn);
                            if (fPrime.getValue() != sfn.getValue()) {
                                summaryFunc[newTe] = fPrime;
                            }
                        }
                    }
                }

                // Line 14-16 (1) short-circuit
                if (targetTabTv.node == callerRet) {
                    auto efn = get_edge_func(te.first, targetTabTv);
                    propagate_ide(te.first, targetTabTv, efn.compose(fn));
                }
            }

            // Line 17-18: (2) if summary edge exists, short-circuit
            for (auto sf : summaryFunc.get_func_by_nodes(te.second.node, callerRet)) {
                auto se = sf.first;
                // Find sfn from <n, d2>, not any <n, d>
                if (se.first.var != te.second.var) continue;
                auto sfn = sf.second;
                // skip T fn
                if (sfn.getValue() == topEnvValue)
                    continue;
                // <s_p, d1> -> <ret_n, d3>
                propagate_ide(te.first, se.second, sfn.compose(fn));
            }
        } else if (hasFlag(dstInfo.flags, VertexFlags::EXIT) &&
                srcProc == dstProc) {
            //auto srcVar = get_vertex_id(te.first);
            //auto dstVar = get_vertex_id(te.second);

            endSummary[te.first].push_back(te.second);

            // Line 20: For every caller
            for (auto inTabTv : get_incoming(te.first)) {

                auto cvit = inTabTv.node;

                for (auto rv : get_return_val(te.second, inTabTv)) {
                    auto sourceTabTv = inTabTv; // <c, d4>
                    auto targetTabTv = rv;      // <ret, d5>

                    // <c, d4> -> <ret, d5>
                    TabEdge newTe = {sourceTabTv, targetTabTv};
                    // Line 22-24
                    // <c, d4> -> <s_p, d1>
                    auto f4 = get_edge_func(sourceTabTv, te.first);
                    // <e_p, d2> -> <r, d5>
                    auto f5 = get_edge_func(te.second, targetTabTv);
                    auto sfn = summaryFunc[newTe];     // XXX
                    auto fPrime = (f5.compose(fn.compose(f4))).may_join(sfn);

                    // Line 25-29
                    if (fPrime.getValue() != sfn.getValue()) {
                        summaryFunc[newTe] = fPrime;

                        // Line 26-29
                        auto callProcName = sgProp->procOf[cvit];
                        for (auto jf : jumpFunc.get_func_by_second(sourceTabTv)) {
                            auto jfn = jf.second;
                            // skip T fn
                            if (jfn.getValue() == topEnvValue)
                                continue;
                            auto je = jf.first;
                            if (sgProp->procOf[je.first.node] == callProcName)
                                propagate_ide(je.first, targetTabTv, fPrime.compose(jfn));
                        }
                    }
                }
            }
        } else {
            std::vector<TabVertex> succ;
            for (auto tb : get_successors(te.second, succ)) {
                auto efn = get_edge_func(te.second, tb);
                propagate_ide(te.first, tb, efn.compose(fn));
            }
        }
    }
    LOG5("=== (END) IDE Tabulate Process ===");
}

VarBitSet Tabulation::may_meet_value(VarBitSet a, VarBitSet b) {
    VarBitSet c = a | b;
    // T (unknown) | v = v
    if (topEnvValue == a) return b;
    if (topEnvValue == b) return a;
    return c;
}

void Tabulation::propagate_value_ide(TabVertex tv, VarBitSet val) {
    LOG5("[II-1] " << dump_tab_vertex(tv) << "&=" << val);

    VarBitSet newVal = may_meet_value(val, valueMap[tv]);
    if (newVal != valueMap[tv]) {
        valueMap[tv] = newVal;
        nodeWorkList.push(tv);
    }
}

void Tabulation::compute_values_ide() {
    nodeWorkList = std::queue<TabVertex>();
    valueMap.clear();
    auto vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (!hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;
        valueMap[get_tab_vertex(*vit)] = topEnvValue;
    }

    // Line 1-14: Phase II-1
    valueMap[rootTv] = VarBitSet(varBitSetSize);
    nodeWorkList.push(rootTv);
    LOG5("=== (BEGIN) IDE Compute Value Process ===");
    while (!nodeWorkList.empty()) {
        // <n, d>
        TabVertex tv = nodeWorkList.front();
        nodeWorkList.pop();

        auto vinfo = (*g)[tv.node];
        auto tvit = get_vertex_id(tv);
        // Line 11-13
        if (hasFlag(vinfo.flags, VertexFlags::CALL)) {
            for (auto [ei, ei_end] = boost::out_edges(tvit, *g);
                    ei != ei_end; ++ei) {
                // <s_q, d'>
                auto targetVar = boost::target(*ei, *g);
                auto targetTabTv = get_tab_vertex(targetVar);
                auto tinfo = (*g)[targetTabTv.node];
                if (hasFlag(tinfo.flags, VertexFlags::ENTRY))
                    propagate_value_ide(targetTabTv,
                            get_edge_func(tv, targetTabTv)(valueMap[tv]));
            }
            continue;
        }

        // Line 8-10: n == s_p
        cstring vProcName = cstring::empty;
        if (sgProp->rootVar == tvit) {
            vProcName = sgProp->procOf[sgProp->rootVar];
        } else if (hasFlag(vinfo.flags, VertexFlags::ENTRY)) {
            if (sgProp->procOf[tv.node] != sgProp->procOf[sgProp->rootVar]) {
                vProcName = sgProp->procOf[tv.node];
            }
        }
        if (vProcName.size() == 0)
            continue;

        // Found the start node of vProcName
        vertices = boost::vertices(*g);
        for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
            auto vinfo = (*g)[*vit];
            // Line 8
            if (hasFlag(vinfo.flags, VertexFlags::CALL) &&
                sgProp->procOf[*vit] == vProcName) {
                // Line 9
                for (auto jf : jumpFunc.get_func_by_nodes(tv.node, *vit)) {
                    auto te = jf.first;
                    auto jfn = jf.second;
                    // <n, d> -> <c, d'>
                    if (te.first == tv && jfn.getValue() != topEnvValue) {
                        // (c, d'), f'(val(s_p, d)): s_p == n
                        propagate_value_ide(te.second, jfn(valueMap[te.first]));
                    }
                }
            }
        }
    }
    // Line 15-17: Phase II-2
    vertices = boost::vertices(*g);
    // For each n, neither CALL nor ENTRY
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        // Skip CALL
        if (hasFlag(vinfo.flags, VertexFlags::CALL)) continue;
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;

        // Set vProcName only for s_p to skip ENTRY
        cstring vProcName = cstring::empty;
        if (sgProp->rootVar == *vit) {
            vProcName = sgProp->procOf[sgProp->rootVar];
        } else if (hasFlag(vinfo.flags, VertexFlags::ENTRY)) {
            // Non-root processes
            if (sgProp->procOf[*vit] != sgProp->procOf[sgProp->rootVar]) {
                vProcName = sgProp->procOf[*vit];
            }
        }
        if (vProcName.size() > 0) continue;

        auto src = sgProp->srcOf[sgProp->procOf[*vit]];

        for (auto jf : jumpFunc.get_func_by_nodes(src, *vit)) {
            auto te = jf.first;     // <s_p, d'>, <n, d>
            auto jfn = jf.second;
            // Skip top-lattice value
            if (jfn.getValue() == topEnvValue)
                continue;

            // val(<n,d>) := val(<n,d>) ^ jf(val(<s_p, d'>))
            auto val = jfn(valueMap[te.first]);
            LOG5("[II-2] " << dump_tab_vertex(te.second) << "&=" << val);
            valueMap[te.second] = may_meet_value(val, valueMap[te.second]);
        }

    }
    LOG5("=== (END) IDE Compute Value Process ===");
}

void Tabulation::dump_result() {
    LOG2("=== X ===");
    for (auto val : valueMap) {
        std::stringstream sstream;
        sstream << dump_tab_vertex(val.first) << ": " << val.second;
        LOG2(cstring(sstream));
    }
}

bool Tabulation::sanity_check_ide() {
    for (auto [ei, ei_end] = boost::edges(*g); ei != ei_end; ++ei) {
        auto &edge = (*g)[*ei];
        if (edge.type != EdgeType::IFDS &&
                edge.type != EdgeType::IFDS_FT)
            continue;

        auto srcTv = get_tab_vertex(boost::source(*ei, *g));
        auto dstTv = get_tab_vertex(boost::target(*ei, *g));

        if (srcTv.var == globalNode && dstTv.var == globalNode) {
            if (edge.fn.has_value()) return false;
        } else if (srcTv.var != globalNode && dstTv.var == globalNode) {
            return false;
        }
    }

    return true;
}

}  // namespace P4::P4StateDependency
