#include "backends/state_dependency/chain_cache.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/json_parser.h"  // reader: P4::JsonData / JsonObject / JsonVector / JsonString / ...
#include "lib/error.h"
#include "lib/hash.h"
#include "lib/json.h"  // writer: P4::Util::JsonObject / JsonArray

namespace P4::P4StateDependency {

namespace {

// v2: cache positions are now captured from POST-midend IR (p4symbex --dump-state-dep-cache) so they
// re-resolve against p4symbex's lowered program. A v1 cache holds PRE-midend positions that a matching
// srcHash would not catch, so the bump makes such caches reject cleanly as a version mismatch.
constexpr int kSchemaVersion = 2;

/// Canonical position key for a node: "line:column:file" (line/column first so the file — which may
/// contain ':' — is the unparsed tail; the key is only ever compared, never split). Printable so the
/// cache is valid JSON. Empty for nodes without valid source info. Computed identically by the writer
/// and the load-time index so the two always agree.
std::string posKey(const IR::Node *node) {
    if (node == nullptr) return "";
    const auto &si = node->getSourceInfo();
    if (!si.isValid()) return "";
    std::ostringstream os;
    os << si.getStart().getLineNumber() << ':' << si.getStart().getColumnNumber() << ':'
       << si.getSourceFile();
    return os.str();
}

/// Walks the program once, indexing every node by posKey. `byPos` (any node at a position — enough
/// because CoverageSet compares (line,col)) re-resolves write/read nodes; `ifByPos` re-resolves the
/// condition node, which is later `->to<IR::IfStatement>()` and so must be that exact type.
class PositionIndex : public Inspector {
 public:
    std::map<std::string, const IR::Node *> byPos;
    std::map<std::string, const IR::IfStatement *> ifByPos;

    bool preorder(const IR::Node *node) override {
        auto k = posKey(node);
        if (!k.empty()) byPos.emplace(k, node);  // first-wins; co-located nodes are srcInfo-equal
        return true;
    }
    bool preorder(const IR::IfStatement *ifs) override {
        auto k = posKey(ifs);
        if (!k.empty()) {
            byPos.emplace(k, ifs);
            ifByPos[k] = ifs;
        }
        return true;
    }
};

// ---- JSON read helpers (over P4::JsonObject from ir/json_parser.h) ----
const JsonData *field(const JsonObject *obj, const char *key) {
    if (obj == nullptr) return nullptr;
    for (const auto &[k, v] : *obj)
        if (k == key) return v.get();
    return nullptr;
}
cstring strField(const JsonObject *obj, const char *key) {
    const auto *f = field(obj, key);
    const auto *s = f != nullptr ? f->to<JsonString>() : nullptr;
    return s != nullptr ? cstring(s->c_str()) : cstring::empty;
}
bool boolField(const JsonObject *obj, const char *key) {
    const auto *f = field(obj, key);
    const auto *b = f != nullptr ? f->to<JsonBoolean>() : nullptr;
    return b != nullptr && b->val;
}
int intField(const JsonObject *obj, const char *key) {
    const auto *f = field(obj, key);
    const auto *n = f != nullptr ? f->to<JsonNumber>() : nullptr;
    return n != nullptr ? static_cast<int>(*n) : 0;
}

std::map<cstring, std::vector<DependencyGraphs::SOChain>> resolveCategory(
    const JsonObject *catObj, const PositionIndex &idx, const std::string &path, bool &ok) {
    std::map<cstring, std::vector<DependencyGraphs::SOChain>> out;
    if (catObj == nullptr) return out;
    for (const auto &[ctrl, chainsVal] : *catObj) {
        const auto *arr = chainsVal->to<JsonVector>();
        if (arr == nullptr) continue;
        std::vector<DependencyGraphs::SOChain> chains;
        for (const auto &chainVal : *arr) {
            const auto *co = chainVal->to<JsonObject>();
            if (co == nullptr) continue;
            DependencyGraphs::SOChain chain;
            chain.id = static_cast<size_t>(intField(co, "id"));
            chain.soName = strField(co, "soName");
            // Older caches serialized an absent table/key as the literal "<null>" (a null cstring's
            // stream form). Normalize it back to empty so isNullOrEmpty holds and a condition-sink
            // chain is not misrouted into the table-sink path. See serializeChainCache.
            auto denull = [](cstring s) { return s == cstring("<null>") ? cstring::empty : s; };
            chain.sinkTableControlPlaneName = denull(strField(co, "sinkTable"));
            chain.sinkKeyName = denull(strField(co, "sinkKey"));
            chain.isUpdate = boolField(co, "isUpdate");
            auto resolveList = [&](const char *key,
                                   std::map<DependencyGraphs::vertex_t, const IR::Node *> &dst) {
                const auto *f = field(co, key);
                const auto *list = f != nullptr ? f->to<JsonVector>() : nullptr;
                if (list == nullptr) return;
                DependencyGraphs::vertex_t i = 0;
                for (const auto &posVal : *list) {
                    const auto *s = posVal->to<JsonString>();
                    if (s == nullptr) continue;
                    auto it = idx.byPos.find(*s);
                    if (it == idx.byPos.end()) {
                        ::P4::error("chain cache %1%: position '%2%' not found in program IR "
                                    "(stale cache?)",
                                    path.c_str(), s->c_str());
                        ok = false;
                        continue;
                    }
                    dst.emplace(i++, it->second);
                }
            };
            resolveList("write", chain.writeNodes);
            resolveList("read", chain.readNodes);
            cstring condPos = strField(co, "cond");
            if (!condPos.isNullOrEmpty()) {
                auto it = idx.ifByPos.find(condPos.string());
                if (it == idx.ifByPos.end()) {
                    ::P4::error("chain cache %1%: condition position '%2%' has no IfStatement in "
                                "program IR (stale cache?)",
                                path.c_str(), condPos);
                    ok = false;
                } else {
                    chain.sinkConditionNode = it->second;
                }
            }
            // --parser-deps pins (optional; absent in baseline/WP caches). Unresolved positions are left
            // null rather than hard-erroring (the pin is a best-effort hint for p4symbex).
            const auto *pdField = field(co, "parserDeps");
            const auto *pdArr = pdField != nullptr ? pdField->to<JsonVector>() : nullptr;
            if (pdArr != nullptr) {
                for (const auto &pdVal : *pdArr) {
                    const auto *pdo = pdVal->to<JsonObject>();
                    if (pdo == nullptr) continue;
                    chain.parserDeps.push_back({strField(pdo, "meta"), strField(pdo, "hdr"),
                                                boolField(pdo, "wp")});
                }
            }
            chains.push_back(std::move(chain));
        }
        out[ctrl] = std::move(chains);
    }
    return out;
}

}  // namespace

cstring computeSourceHash(const std::string &p4File, cstring arch, cstring langVersion) {
    std::ifstream in(p4File, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string blob = ss.str();
    blob += "|arch=";
    blob += std::string(arch.string_view());
    blob += "|lang=";
    blob += std::string(langVersion.string_view());
    uint64_t h = Util::hash(blob.data(), blob.size());
    std::ostringstream hs;
    hs << std::hex << h;
    return cstring(hs.str());
}

void serializeChainCache(const StateDependencyResult &result, const std::string &path,
                         cstring srcHash, cstring arch) {
    auto chainToJson = [](const DependencyGraphs::SOChain &chain) -> Util::JsonObject * {
        auto *o = new Util::JsonObject();
        o->emplace("id", chain.id);
        o->emplace("soName", chain.soName);
        // A null cstring streams as the literal "<null>" (lib/cstring.h), which reads back as a
        // NON-empty string and would misroute a condition-sink chain (empty sinkTable) into the
        // table-sink path. Emit "" for an absent table/key so the loader's isNullOrEmpty holds.
        o->emplace("sinkTable", chain.sinkTableControlPlaneName.isNullOrEmpty()
                                    ? cstring::empty
                                    : chain.sinkTableControlPlaneName);
        o->emplace("sinkKey", chain.sinkKeyName.isNullOrEmpty() ? cstring::empty : chain.sinkKeyName);
        o->emplace("isUpdate", chain.isUpdate);
        auto *w = new Util::JsonArray();
        for (const auto &[v, node] : chain.writeNodes) {
            auto k = posKey(node);
            if (!k.empty()) w->append(cstring(k));
        }
        o->emplace("write", w);
        auto *r = new Util::JsonArray();
        for (const auto &[v, node] : chain.readNodes) {
            auto k = posKey(node);
            if (!k.empty()) r->append(cstring(k));
        }
        o->emplace("read", r);
        o->emplace("cond", cstring(posKey(chain.sinkConditionNode)));  // "" when no condition sink
        // --parser-deps: per-chain header pins (optional/additive; absent in baseline/WP caches, so old
        // caches still load with an empty parserDeps).
        if (!chain.parserDeps.empty()) {
            auto *pd = new Util::JsonArray();
            for (const auto &dep : chain.parserDeps) {
                auto *d = new Util::JsonObject();
                d->emplace("meta", dep.metaPath);   // field-path strings (stable across tools/unroll)
                d->emplace("hdr", dep.hdrPath);
                d->emplace("wp", dep.writePath);
                pd->append(d);
            }
            o->emplace("parserDeps", pd);
        }
        return o;
    };
    auto catToJson = [&](const std::map<cstring, std::vector<DependencyGraphs::SOChain>> &cat) {
        auto *byCtrl = new Util::JsonObject();
        for (const auto &[ctrl, chains] : cat) {
            auto *arr = new Util::JsonArray();
            for (const auto &chain : chains) arr->append(chainToJson(chain));
            byCtrl->emplace(ctrl, arr);
        }
        return byCtrl;
    };

    auto *root = new Util::JsonObject();
    root->emplace("version", kSchemaVersion);
    root->emplace("srcHash", srcHash);
    root->emplace("arch", arch);
    root->emplace("key", catToJson(result.dataWriteKeyChains));
    root->emplace("cond", catToJson(result.dataWriteCondChains));

    std::ofstream out(path);
    if (!out) {
        ::P4::error("chain cache: cannot open %1% for writing", path.c_str());
        return;
    }
    root->serialize(out);
}

StateDependencyResult loadChainCache(const std::string &path, const IR::P4Program *program,
                                     cstring srcHash, cstring arch) {
    StateDependencyResult result;
    std::ifstream in(path);
    if (!in) {
        ::P4::error("--state-dep-cache: cache file %1% not found", path.c_str());
        return result;
    }
    std::unique_ptr<JsonData> json;
    in >> json;
    const auto *root = json != nullptr ? json->to<JsonObject>() : nullptr;
    if (root == nullptr) {
        ::P4::error("--state-dep-cache: %1% is not valid JSON", path.c_str());
        return result;
    }
    if (intField(root, "version") != kSchemaVersion) {
        ::P4::error("--state-dep-cache: %1% schema version mismatch", path.c_str());
        return result;
    }
    cstring fileHash = strField(root, "srcHash");
    cstring fileArch = strField(root, "arch");
    if (fileHash != srcHash || fileArch != arch) {
        ::P4::error("--state-dep-cache: %1% does not match this program (srcHash %2% vs %3%, "
                    "arch %4% vs %5%); rebuild the cache",
                    path.c_str(), fileHash, srcHash, fileArch, arch);
        return result;
    }

    PositionIndex idx;
    program->apply(idx);

    bool ok = true;
    const auto *keyObj = field(root, "key");
    const auto *condObj = field(root, "cond");
    result.dataWriteKeyChains =
        resolveCategory(keyObj != nullptr ? keyObj->to<JsonObject>() : nullptr, idx, path, ok);
    result.dataWriteCondChains =
        resolveCategory(condObj != nullptr ? condObj->to<JsonObject>() : nullptr, idx, path, ok);
    if (!ok) ::P4::error("--state-dep-cache: %1% could not be fully re-resolved", path.c_str());
    return result;
}

}  // namespace P4::P4StateDependency
