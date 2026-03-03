#include "tabulation.h"
#include "graphs.h"

namespace P4::P4StateDependency {

using TabVertex = Tabulation::TabVertex;

void Tabulation::init() {
    pathEdge.clear();
    workList = std::queue<TabEdge>();
    summaryEdge.clear();

    pathEdge.insert({rootTv, rootTv});
    workList.push({rootTv, rootTv});
}

void Tabulation::propagate(TabVertex a, TabVertex b) {
    LOG5(dump_tab_vertex(a) << "->" << dump_tab_vertex(b));

    TabEdge ab = {a, b};
    if (pathEdge.find(ab) == pathEdge.end()) {
        pathEdge.insert(ab);
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

void Tabulation::forward_tabulate() {
    LOG5("=== (BEGIN) Tabulate Process ===");
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
                    propagate(targetTabTv, targetTabTv);
                }

                // Line 17-19 (1) short-circuit
                if (targetTabTv.node == callerRet) {
                    propagate(te.first, targetTabTv);
                }
            }

            // Line 17-19: (2) if summary edge exists, short-circuit
            for (auto se : summaryEdge) {
                if (se.first.node == te.second.node &&
                        se.second.node == callerRet) {
                    propagate(te.first, se.second);
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
                    auto edge = (*g)[*ei];
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
                                    propagate(pe.first, targetTabTv);
                            }
                        }
                    }
                }
            }
        } else {
            std::vector<TabVertex> succ;
            for (auto tb : get_successors(te.second, succ)) {
                propagate(te.first, tb);
            }
        }
    }
    LOG5("=== (END) Tabulate Process ===");

    LOG2("=== Meet-Over-All-Valid-Paths Solutions ===\n");
    for (auto pe : pathEdge) {
        LOG2(dump_tab_vertex(pe.first) << "->" << dump_tab_vertex(pe.second));
        auto varVit = get_vertex_id(pe.second);
        auto &varInfo = (*g)[varVit];
        varInfo.color = "black"_cs;
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
        std::vector<const IR::Node *> reachableNodes;
        for (auto pe : pathEdge) {
            auto firstProcName = sgProp->procOf[pe.first.node];
            if (firstProcName == vProcName && pe.second.node == *vit) {
                reachableNodes.push_back(pe.second.var);
            }
        }

        if (reachableNodes.size() > 0) {
            std::stringstream sstream;
            sstream << "  " << vinfo.name << "(" << *vit << "):";
            for (auto rn : reachableNodes) {
                sstream << " " << rn;
            }
            LOG2(cstring(sstream));
        }
    }
}

}  // namespace P4::P4StateDependency
