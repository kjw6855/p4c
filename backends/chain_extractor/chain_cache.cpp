#include "backends/chain_extractor/chain_cache.h"

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

namespace P4::P4ChainExtractor {

namespace {

// v2: cache positions are now captured from POST-midend IR (p4symbex --dump-state-dep-cache) so they
// re-resolve against p4symbex's lowered program. A v1 cache holds PRE-midend positions that a matching
// srcHash would not catch, so the bump makes such caches reject cleanly as a version mismatch.
// v3: the H2S2K/H2S2C categories are named "h2s2k"/"h2s2c" (v2: "key"/"cond"), so all ten categories
// read {h,a}2s2{k,c,o,outk,outc}. v2 caches still load (p4csd and old nightly caches are v2).
constexpr int kSchemaVersion = 3;
constexpr int kLegacySchemaVersion = 2;

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
            // Output-chain hops (optional; only in the outk/outc categories).
            const auto *hopsField = field(co, "hops");
            const auto *hopsArr = hopsField != nullptr ? hopsField->to<JsonVector>() : nullptr;
            if (hopsArr != nullptr) {
                for (const auto &hopVal : *hopsArr) {
                    const auto *ho = hopVal->to<JsonObject>();
                    if (ho == nullptr) continue;
                    DependencyGraphs::SOChain::Hop hop{strField(ho, "table"), {}};
                    const auto *selField = field(ho, "select");
                    const auto *selArr = selField != nullptr ? selField->to<JsonVector>() : nullptr;
                    if (selArr != nullptr)
                        for (const auto &a : *selArr)
                            if (const auto *str = a->to<JsonString>())
                                hop.select.push_back(cstring(str->c_str()));
                    chain.hops.push_back(std::move(hop));
                }
            }
            chains.push_back(std::move(chain));
        }
        out[ctrl] = std::move(chains);
    }
    return out;
}

/// The chains of category @name of the cache @root, re-resolved against @idx (see resolveCategory).
std::map<cstring, std::vector<DependencyGraphs::SOChain>> loadCategory(
    const JsonObject *root, const char *name, const PositionIndex &idx, const std::string &path,
    bool &ok) {
    const auto *obj = field(root, name);
    return resolveCategory(obj != nullptr ? obj->to<JsonObject>() : nullptr, idx, path, ok);
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
        // Output chains: the tables (and their actions) steering the value to the sink.
        if (!chain.hops.empty()) {
            auto *hops = new Util::JsonArray();
            for (const auto &hop : chain.hops) {
                auto *h = new Util::JsonObject();
                h->emplace("table", hop.table);
                auto *select = new Util::JsonArray();
                for (const auto &a : hop.select) select->append(a);
                h->emplace("select", select);
                hops->append(h);
            }
            o->emplace("hops", hops);
        }
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
    // @withSinkVar: also record the sink variable, for chains ending at a variable rather than at
    // a table key or condition (the output relations *2S2O).
    auto catToJson = [&](const std::map<cstring, std::vector<DependencyGraphs::SOChain>> &cat,
                         bool withSinkVar = false) {
        auto *byCtrl = new Util::JsonObject();
        for (const auto &[ctrl, chains] : cat) {
            auto *arr = new Util::JsonArray();
            for (const auto &chain : chains) {
                auto *o = chainToJson(chain);
                if (withSinkVar) {
                    const auto *var = chain.sinkNode.esgId.second;
                    o->emplace("sinkVar", var != nullptr ? cstring(var->toString()) : cstring::empty);
                }
                arr->append(o);
            }
            byCtrl->emplace(ctrl, arr);
        }
        return byCtrl;
    };

    auto *root = new Util::JsonObject();
    root->emplace("version", kSchemaVersion);
    root->emplace("srcHash", srcHash);
    root->emplace("arch", arch);
    root->emplace("h2s2k", catToJson(result.dataWriteKeyChains));
    root->emplace("h2s2c", catToJson(result.dataWriteCondChains));
    root->emplace("a2s2k", catToJson(result.dataWriteA2SKeyChains));
    root->emplace("a2s2c", catToJson(result.dataWriteA2SCondChains));
    root->emplace("h2s2o", catToJson(result.dataWriteOutChains, true));
    root->emplace("h2s2outc", catToJson(result.dataWriteOutCondChains));
    root->emplace("a2s2o", catToJson(result.dataWriteA2SOutChains, true));
    root->emplace("a2s2outc", catToJson(result.dataWriteA2SOutCondChains));
    root->emplace("h2s2outk", catToJson(result.dataWriteOutKeyChains));
    root->emplace("a2s2outk", catToJson(result.dataWriteA2SOutKeyChains));

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
    const int version = intField(root, "version");
    if (version != kSchemaVersion && version != kLegacySchemaVersion) {
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
    const bool legacy = version == kLegacySchemaVersion;
    result.dataWriteKeyChains = loadCategory(root, legacy ? "key" : "h2s2k", idx, path, ok);
    result.dataWriteCondChains = loadCategory(root, legacy ? "cond" : "h2s2c", idx, path, ok);
    result.dataWriteA2SKeyChains = loadCategory(root, "a2s2k", idx, path, ok);
    result.dataWriteA2SCondChains = loadCategory(root, "a2s2c", idx, path, ok);
    result.dataWriteOutChains = loadCategory(root, "h2s2o", idx, path, ok);
    result.dataWriteOutCondChains = loadCategory(root, "h2s2outc", idx, path, ok);
    result.dataWriteA2SOutChains = loadCategory(root, "a2s2o", idx, path, ok);
    result.dataWriteA2SOutCondChains = loadCategory(root, "a2s2outc", idx, path, ok);
    result.dataWriteOutKeyChains = loadCategory(root, "h2s2outk", idx, path, ok);
    result.dataWriteA2SOutKeyChains = loadCategory(root, "a2s2outk", idx, path, ok);
    if (!ok) ::P4::error("--state-dep-cache: %1% could not be fully re-resolved", path.c_str());
    return result;
}

}  // namespace P4::P4ChainExtractor
