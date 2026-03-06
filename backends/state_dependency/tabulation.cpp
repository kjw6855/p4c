#include "tabulation.h"
#include "graphs.h"

namespace P4::P4StateDependency {

using TabVertex = Tabulation::TabVertex;

void Tabulation::init_ifds() {
    pathEdge.clear();
    workList = std::queue<TabEdge>();
    summaryEdge.clear();

    pathEdge.insert({rootTv, rootTv});
    workList.push({rootTv, rootTv});
}

void Tabulation::init_ide() {
    workList = std::queue<TabEdge>();
    jumpFunc.clear();
    summaryFunc.clear();

    BUG_CHECK(sgProp->actionIdMap.size() > 0, "Action does not exist.");

    auto topFunc = EdgeFuncHolder(std::make_unique<TopFunc>(sgProp->actionIdMap.size()));

    auto vertices = boost::vertices(*g);

    // Line 1-2: Init jumpFunc
    vertices = boost::vertices(*g);
    for (auto &vit = vertices.first; vit != vertices.second; ++vit) {
        auto vinfo = (*g)[*vit];
        if (hasFlag(vinfo.flags, VertexFlags::VARIABLE)) continue;
        auto vProcName = sgProp->procOf[*vit];
        // 1) main process
        if (vProcName == sgProp->procOf[sgProp->rootVar]) {
            for (auto *p : sgProp->variableList)
                jumpFunc[{rootTv, TabVertex{*vit, p}}] = topFunc;
        } else {
            auto src = sgProp->srcOf[vProcName];
            for (auto *p : sgProp->variableList)
                for (auto *q : sgProp->variableList)
                    jumpFunc[{TabVertex{src, p}, TabVertex{*vit, q}}] = topFunc;
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
                jumpFunc[{srcTv, dstTv}] = topFunc;

        } else if (edge.type == EdgeType::CALL_TO_RETURN) {
            auto src = boost::source(*ei, *g);
            auto dst = boost::target(*ei, *g);

            for (auto *p : sgProp->variableList)
                for (auto *q : sgProp->variableList)
                    summaryFunc[{TabVertex{src, p}, TabVertex{dst, q}}] = topFunc;
        }
    }

    workList.push({rootTv, rootTv});
    jumpFunc[{rootTv, rootTv}] = globalIdFunc;
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
            auto callMapIt = sgProp->callMap.find(te.second.node);
            if (callMapIt == sgProp->callMap.end()) {
                std::stringstream sstream;
                sstream << dstInfo.node;
                BUG("No callMap for %1% (%2%)",
                        cstring(sstream), te.second.node);
            }
            auto [calleeEntry, callerRet] = callMapIt->second;

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
                        auto [_, retSite] = sgProp->callMap[cvit];
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
            auto callMapIt = sgProp->callMap.find(te.second.node);
            if (callMapIt == sgProp->callMap.end()) {
                std::stringstream sstream;
                sstream << dstInfo.node;
                BUG("No callMap for %1% (%2%)",
                        cstring(sstream), te.second.node);
            }
            auto [calleeEntry, callerRet] = callMapIt->second;

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
            for (auto sf : summaryFunc) {
                auto sfn = sf.second;
                // skip T fn
                if (sfn.getValue() == sgProp->topEnvValue)
                    continue;
                auto se = sf.first;
                if (se.first.node == te.second.node &&
                        se.second.node == callerRet) {
                    propagate_ide(te.first, se.second, sfn.compose(fn));
                }
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
                        auto [_, retSite] = sgProp->callMap[cvit];
                        if (targetTabTv.node != retSite) continue;

                        TabEdge te = {sourceTabTv, targetTabTv};
                        // Line 22-24
                        auto f4 = get_edge_func(sourceTabTv, te.first);
                        auto f5 = get_edge_func(te.second, targetTabTv);
                        auto sfn = summaryFunc[te];     // XXX
                        auto fPrime = (f5.compose(fn.compose(f4))).may_join(sfn);

                        // Line 25-29
                        if (fPrime.getValue() != sfn.getValue()) {
                            summaryFunc[te] = fPrime;
                        }
                        if (summaryEdge.find(te) == summaryEdge.end()) {
                            summaryEdge.insert(te);
                            // Line 26-29
                            auto callProcName = sgProp->procOf[cvit];
                            for (auto jf : jumpFunc) {
                                auto jfn = jf.second;
                                // skip T fn
                                if (jfn.getValue() == sgProp->topEnvValue)
                                    continue;
                                auto je = jf.first;
                                if (je.second.node == sourceTabTv.node &&
                                        je.second.var == sourceTabTv.var &&
                                        sgProp->procOf[je.first.node] == callProcName)
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

size_t Tabulation::may_meet_value(size_t a, size_t b) {
    size_t c = a | b;
    // T (unknown) | v = v
    if (sgProp->topEnvValue == a) return b;
    if (sgProp->topEnvValue == b) return a;
    return c;
}

void Tabulation::propagate_value_ide(TabVertex tv, size_t val) {
    LOG5("[II-1] " << dump_tab_vertex(tv) << "&=" << val);

    size_t newVal = may_meet_value(val, valueMap[tv]);
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
        valueMap[get_tab_vertex(*vit)] = sgProp->topEnvValue;
    }

    // Line 1-14: Phase II-1
    valueMap[rootTv] = 0;
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
                for (auto jf : jumpFunc) {
                    auto te = jf.first;
                    auto jfn = jf.second;
                    // <n, d> -> <c, d'>
                    if (te.first == tv && te.second.node == *vit &&
                            jfn.getValue() != sgProp->topEnvValue) {
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

        for (auto jf : jumpFunc) {
            auto te = jf.first;     // <s_p, d'>, <n, d>
            auto jfn = jf.second;
            // Find jumpFn with dst n(*vit)
            if (te.second.node != *vit) continue;
            // Skip top-lattice value
            if (jfn.getValue() == sgProp->topEnvValue)
                continue;
            auto vProcName = sgProp->procOf[*vit];
            auto src = sgProp->srcOf[vProcName];
            // Skip non-source s_p
            if (src != te.first.node) continue;

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

}  // namespace P4::P4StateDependency
