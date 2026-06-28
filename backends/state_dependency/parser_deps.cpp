#include "backends/state_dependency/parser_deps.h"

#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "lib/log.h"

namespace P4::P4StateDependency {

// Defined in controls.cpp (same library/namespace): true if @type is a struct containing header fields.
bool isHeaderStruct(const IR::Type *type, const TypeMap *typeMap);

namespace {

// Field-path string for a node, matching Graphs::get_var_name (so record keys equal control var names).
cstring nodePath(const IR::Node *n) {
    std::stringstream ss;
    ss << n;
    cstring full(ss.str());
    if (auto *p = full.findlast(' ')) return cstring(p + 1);
    return full;
}

// Collect maximal field references (Member / PathExpression / ArrayIndex) within an expression.
struct RefCollector : public Inspector {
    std::vector<const IR::Expression *> refs;
    bool preorder(const IR::Member *m) override { refs.push_back(m); return false; }
    bool preorder(const IR::PathExpression *pe) override { refs.push_back(pe); return false; }
    bool preorder(const IR::ArrayIndex *ai) override { refs.push_back(ai); return false; }
};

// Collect every `lhs = rhs` across all parser states (flow-insensitive may-analysis).
struct AssignCollector : public Inspector {
    std::map<cstring, std::vector<std::pair<cstring, const IR::Node *>>> deps;  // lhsPath -> {(refPath,node)}
    std::map<cstring, const IR::Node *> lhsNode;

    bool preorder(const IR::BaseAssignmentStatement *as) override {
        cstring lp = nodePath(as->left);
        lhsNode[lp] = as->left;
        RefCollector rc;
        as->right->apply(rc);
        auto &v = deps[lp];
        for (auto *r : rc.refs) v.emplace_back(nodePath(r), r);
        return false;
    }
};

}  // namespace

ParserDepsRecord computeParserDeps(const IR::P4Parser *parser, P4::ReferenceMap * /*refMap*/,
                                   P4::TypeMap *typeMap) {
    ParserDepsRecord out;

    std::set<std::string> headerNames;
    for (auto *p : parser->getApplyParameters()->parameters) {
        auto *t = typeMap->getType(p, true);
        if (t != nullptr && isHeaderStruct(t, typeMap)) headerNames.insert(p->name.name.string());
    }
    if (headerNames.empty()) return out;  // no header param -> nothing to source

    auto isHeaderPath = [&](cstring path) {
        std::string s = path.string();
        for (const auto &h : headerNames)
            if (s == h || (s.size() > h.size() && s.compare(0, h.size(), h) == 0 && s[h.size()] == '.'))
                return true;
        return false;
    };

    AssignCollector ac;
    parser->apply(ac);

    // Transitive header-provenance with memoization + cycle guard.
    std::map<cstring, std::vector<ParserHeaderDep>> memo;
    std::set<cstring> inProgress;
    std::function<std::vector<ParserHeaderDep>(cstring)> prov =
        [&](cstring path) -> std::vector<ParserHeaderDep> {
        auto mit = memo.find(path);
        if (mit != memo.end()) return mit->second;
        if (inProgress.count(path)) return {};
        inProgress.insert(path);
        std::vector<ParserHeaderDep> result;
        std::set<cstring> seen;
        auto dit = ac.deps.find(path);
        if (dit != ac.deps.end()) {
            for (auto &[refPath, refNode] : dit->second) {
                if (isHeaderPath(refPath)) {
                    if (seen.insert(refPath).second) result.push_back({refPath, refNode});
                } else {
                    for (auto &hd : prov(refPath))
                        if (seen.insert(hd.headerPath).second) result.push_back(hd);
                }
            }
        }
        inProgress.erase(path);
        memo[path] = result;
        return result;
    };

    for (auto &[lhsPath, refs] : ac.deps) {
        (void)refs;
        if (isHeaderPath(lhsPath)) continue;  // only metadata fields become sources
        auto hdrs = prov(lhsPath);
        if (!hdrs.empty()) {
            out.metaToHeaders[lhsPath] = hdrs;
            out.metaNode[lhsPath] = ac.lhsNode[lhsPath];
            LOG2("[parser-deps] " << lhsPath << " <- " << hdrs.size() << " header(s)");
        }
    }
    return out;
}

void attachParserDeps(DependencyGraphs::SOChain &chain, const ParserDepsRecord &rec) {
    if (rec.metaToHeaders.empty()) return;
    std::set<std::tuple<cstring, cstring, bool>> seen;
    auto scan = [&](const auto &nodes, bool writePath) {
        for (auto &kv : nodes) {
            const IR::Node *node = kv.second;
            if (node == nullptr) continue;
            RefCollector rc;
            node->apply(rc);
            for (auto *r : rc.refs) {
                cstring path = nodePath(r);
                auto it = rec.metaToHeaders.find(path);
                if (it == rec.metaToHeaders.end()) continue;
                for (const auto &hd : it->second) {
                    if (seen.insert(std::make_tuple(path, hd.headerPath, writePath)).second)
                        chain.parserDeps.push_back({path, hd.headerPath, writePath});
                }
            }
        }
    };
    scan(chain.writeNodes, true);
    scan(chain.readNodes, false);
}

}  // namespace P4::P4StateDependency
