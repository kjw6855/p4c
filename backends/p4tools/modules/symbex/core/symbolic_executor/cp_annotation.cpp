#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_annotation.h"

#include <fstream>
#include <regex>

#include "ir/json_parser.h"
#include "lib/error.h"

namespace P4::P4Tools::Symbex {

namespace {

// ---- JSON read helpers (mirror backends/state_dependency/chain_cache.cpp:63-90) ----
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

/// Parse one clause into the enforceable subset. Unrecognized text is NOT an error: the file is
/// also documentation, and rejecting it would push authors toward writing less provenance.
CpAssumeClause parseClause(cstring raw) {
    CpAssumeClause c;
    c.raw = raw;
    const std::string s(raw.string_view());
    // A guarded clause ("X implies Y") is retained but not enforced in v1 - enforcing only the
    // consequent would be UNSOUND (it would constrain paths the guard never selected).
    if (s.find(" implies ") != std::string::npos) return c;

    static const std::regex kDefault(R"(^\s*default_action\s*\(\s*([\w.]+)\s*\)\s*==\s*([\w.]+)\s*$)");
    static const std::regex kAction(R"(^\s*action\s*\(\s*([\w.]+)\s*\)\s*(==|!=)\s*([\w.]+)\s*$)");
    static const std::regex kHitMiss(R"(^\s*(hit|miss)\s*\(\s*([\w.]+)\s*\)\s*$)");
    std::smatch m;
    if (std::regex_match(s, m, kDefault)) {
        c.kind = CpAssumeClause::Kind::DefaultAction;
        c.table = cstring(m[1].str());
        c.action = cstring(m[2].str());
    } else if (std::regex_match(s, m, kAction)) {
        c.kind = m[2].str() == "==" ? CpAssumeClause::Kind::ActionEq : CpAssumeClause::Kind::ActionNeq;
        c.table = cstring(m[1].str());
        c.action = cstring(m[3].str());
    } else if (std::regex_match(s, m, kHitMiss)) {
        c.kind = m[1].str() == "hit" ? CpAssumeClause::Kind::Hit : CpAssumeClause::Kind::Miss;
        c.table = cstring(m[2].str());
    }
    return c;
}

}  // namespace

std::optional<CpAnnotation> CpAnnotation::load(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        ::P4::error("--cp-annotation: file %1% not found", path.c_str());
        return std::nullopt;
    }
    std::unique_ptr<JsonData> json;
    in >> json;
    const auto *root = json != nullptr ? json->to<JsonObject>() : nullptr;
    if (root == nullptr) {
        ::P4::error("--cp-annotation: %1% is not valid JSON", path.c_str());
        return std::nullopt;
    }

    CpAnnotation a;
    a.program_ = strField(root, "program");
    if (a.program_.isNullOrEmpty()) {
        ::P4::error("--cp-annotation: %1% has no \"program\" field", path.c_str());
        return std::nullopt;
    }
    if (const auto *tm = field(root, "threat_model"); tm != nullptr) {
        const auto *o = tm->to<JsonObject>();
        a.threatModel_ = o != nullptr ? strField(o, "model") : cstring::empty;
    }

    // port_roles: { role: { "ports": "abstract" | [ints], ... } }
    if (const auto *pr = field(root, "port_roles"); pr != nullptr) {
        if (const auto *o = pr->to<JsonObject>(); o != nullptr) {
            for (const auto &[role, val] : *o) {
                std::set<int> ports;
                const auto *ro = val->to<JsonObject>();
                const auto *pv = ro != nullptr ? field(ro, "ports") : nullptr;
                if (const auto *arr = pv != nullptr ? pv->to<JsonVector>() : nullptr; arr != nullptr) {
                    for (const auto &e : *arr)
                        if (const auto *num = e->to<JsonNumber>(); num != nullptr)
                            ports.insert(static_cast<int>(*num));
                }
                if (!ports.empty()) a.hasConcretePorts_ = true;
                a.portRoles_[cstring(role)] = std::move(ports);
            }
        }
    }

    // registers: { soName: { writable_by[], partitioned_by, shared } }
    if (const auto *rg = field(root, "registers"); rg != nullptr) {
        if (const auto *o = rg->to<JsonObject>(); o != nullptr) {
            for (const auto &[so, val] : *o) {
                const auto *ro = val->to<JsonObject>();
                if (ro == nullptr) continue;
                CpRegisterRule rule;
                rule.partitionedBy = strField(ro, "partitioned_by");
                rule.shared = boolField(ro, "shared");
                if (const auto *wb = field(ro, "writable_by"); wb != nullptr) {
                    if (const auto *arr = wb->to<JsonVector>(); arr != nullptr) {
                        for (const auto &e : *arr)
                            if (const auto *s = e->to<JsonString>(); s != nullptr)
                                rule.writableBy.emplace_back(s->c_str());
                    }
                }
                a.registers_[cstring(so)] = std::move(rule);
            }
        }
    }

    // assume: [ { clause, source, ref, reason } ]  (older files may use plain strings)
    if (const auto *as = field(root, "assume"); as != nullptr) {
        if (const auto *arr = as->to<JsonVector>(); arr != nullptr) {
            for (const auto &e : *arr) {
                if (const auto *s = e->to<JsonString>(); s != nullptr) {
                    a.assume_.push_back(parseClause(cstring(s->c_str())));
                    continue;
                }
                const auto *o = e->to<JsonObject>();
                if (o == nullptr) continue;
                auto c = parseClause(strField(o, "clause"));
                c.source = strField(o, "source");
                c.ref = strField(o, "ref");
                c.reason = strField(o, "reason");
                a.assume_.push_back(std::move(c));
            }
        }
    }
    return a;
}

std::set<cstring> CpAnnotation::rolesForPort(int port) const {
    std::set<cstring> out;
    for (const auto &[role, ports] : portRoles_)
        if (ports.count(port) > 0) out.insert(role);
    return out;
}

const CpRegisterRule *CpAnnotation::registerRule(cstring soName) const {
    auto it = registers_.find(soName);
    if (it != registers_.end()) return &it->second;
    // SO names appear both fully qualified ("ingress.registerRound") and bare ("pit_r"); accept
    // either by matching on the trailing component.
    const std::string want(soName.string_view());
    const auto tail = [](const std::string &s) {
        auto p = s.find_last_of('.');
        return p == std::string::npos ? s : s.substr(p + 1);
    };
    const std::string wantTail = tail(want);
    for (const auto &[k, v] : registers_)
        if (tail(std::string(k.string_view())) == wantTail) return &v;
    return nullptr;
}

std::vector<const CpAssumeClause *> CpAnnotation::clausesFor(cstring table) const {
    std::vector<const CpAssumeClause *> out;
    const std::string want(table.string_view());
    const auto tail = [](const std::string &s) {
        auto p = s.find_last_of('.');
        return p == std::string::npos ? s : s.substr(p + 1);
    };
    for (const auto &c : assume_) {
        if (c.kind == CpAssumeClause::Kind::Unparsed || c.table.isNullOrEmpty()) continue;
        if (c.table == table || tail(std::string(c.table.string_view())) == tail(want))
            out.push_back(&c);
    }
    return out;
}

}  // namespace P4::P4Tools::Symbex
