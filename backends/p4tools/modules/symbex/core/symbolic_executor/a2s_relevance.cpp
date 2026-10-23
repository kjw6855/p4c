#include "backends/p4tools/modules/symbex/core/symbolic_executor/a2s_relevance.h"

#include <fstream>
#include <optional>

#include "backends/p4tools/modules/symbex/p4symbex_bfrt.pb.h"

namespace P4::P4Tools::Symbex {

namespace {

using SOChain = P4ChainExtractor::DependencyGraphs::SOChain;

const char *typeName(int type) {
    switch (type) {
        case symbex::tofino::CP_UPDATE_INSERT:
            return "INSERT";
        case symbex::tofino::CP_UPDATE_MODIFY:
            return "MODIFY";
        case symbex::tofino::CP_UPDATE_DELETE:
            return "DELETE";
        default:
            return "UNSPECIFIED";
    }
}

/// A param value as a2s_sources.py prints it (decimal; None when the rule has no such param).
std::string paramText(const std::optional<CpSnapshotRule> &rule, cstring param) {
    if (!rule.has_value()) return "None";
    auto it = rule->params.find(param);
    return it == rule->params.end() ? std::string("None") : it->second.str();
}

/// Why @p u changes cp source @p src, or nullopt.
std::optional<std::string> touches(const CpStepUpdate &u, const SOChain::CPSource &src) {
    const bool tableMatches = std::any_of(src.tables.begin(), src.tables.end(), [&u](cstring t) {
        return cpNamesMatch(u.table.string_view(), t.string_view());
    });
    if (!tableMatches) return std::nullopt;
    bool actionMatches = false;
    for (const auto *rule : {u.after ? &*u.after : nullptr, u.before ? &*u.before : nullptr}) {
        if (rule != nullptr && !rule->action.isNullOrEmpty() &&
            cpNamesMatch(rule->action.string_view(), src.action.string_view())) {
            actionMatches = true;
        }
    }
    if (!actionMatches) return std::nullopt;
    if (u.type != symbex::tofino::CP_UPDATE_MODIFY || !u.before.has_value()) {
        return std::string(typeName(u.type)) + " of a " + src.action.string() + " entry";
    }
    if (u.before->action != u.after->action) {
        return "action " + u.before->action.string() + " -> " + u.after->action.string();
    }
    const auto before = paramText(u.before, src.param);
    const auto after = paramText(u.after, src.param);
    if (before != after) return src.param.string() + " " + before + " -> " + after;
    return std::nullopt;
}

}  // namespace

std::vector<A2SRelevance> a2sRelevance(const SOChain &chain, cstring category, cstring stepId,
                                       const std::vector<CpStepUpdate> &updates) {
    std::vector<A2SRelevance> out;
    std::vector<cstring> kinds;
    if (chain.cp.inconsistency()) kinds.push_back("inconsistency"_cs);
    if (chain.cp.residual()) kinds.push_back("residual"_cs);
    auto row = [&](cstring kind, const CpStepUpdate &u, cstring action, cstring param,
                   std::string reason) {
        out.push_back({chain.id, category, kind, stepId, u.index, u.table, action, param,
                       std::move(reason)});
    };
    for (const auto &u : updates) {
        if (u.isRegister) {
            if (cpNamesMatch(u.table.string_view(), chain.soName.string_view())) {
                for (auto kind : kinds) {
                    row(kind, u, ""_cs, ""_cs, "register cell of " + chain.soName.string());
                }
            }
            continue;
        }
        if (chain.cp.inconsistency()) {
            for (const auto &src : chain.cp.writeData) {
                if (auto why = touches(u, src)) row("inconsistency"_cs, u, src.action, src.param, *why);
            }
        }
        if (chain.cp.residual()) {
            for (const auto *srcs : {&chain.cp.writeIdx, &chain.cp.readIdx}) {
                for (const auto &src : *srcs) {
                    if (auto why = touches(u, src)) row("residual"_cs, u, src.action, src.param, *why);
                }
            }
        }
    }
    return out;
}

void writeA2SRelevanceCsv(const std::string &path, const std::vector<A2SRelevance> &rows) {
    std::ofstream out(path);
    out << "chain_id,category,kind,step,update_index,table,action,param,reason\r\n";
    for (const auto &r : rows) {
        out << r.chainId << "," << r.category << "," << r.kind << "," << r.step << ","
            << r.updateIndex << "," << r.table << "," << r.action << "," << r.param << ","
            << r.reason << "\r\n";
    }
}

}  // namespace P4::P4Tools::Symbex
