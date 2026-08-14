#include "backends/p4tools/modules/symbex/core/symbolic_executor/cp_annotation.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <stdexcept>

#include "backends/p4tools/modules/symbex/options.h"
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

/// Numbers arrive as JSON numbers or as strings (dotted-quad and hex are written as strings in the
/// artifacts). Returns false when @p key is absent or unreadable, so callers can tell "0" from
/// "missing" - the difference between an Eq-to-zero term and a malformed one.
bool numField(const JsonObject *obj, const char *key, big_int *out) {
    const auto *f = field(obj, key);
    if (f == nullptr) return false;
    if (const auto *n = f->to<JsonNumber>(); n != nullptr) {
        *out = big_int(static_cast<int64_t>(*n));
        return true;
    }
    const auto *s = f->to<JsonString>();
    if (s == nullptr) return false;
    std::string v(s->c_str());
    try {
        if (v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0) {
            *out = big_int(0);
            for (size_t i = 2; i < v.size(); ++i) {
                const char c = v[i];
                const int d = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
                if (d < 0 || d > 15) return false;
                *out = *out * 16 + d;
            }
            return true;
        }
        if (v.find('.') != std::string::npos) {  // dotted quad -> 32-bit
            big_int acc = 0;
            size_t pos = 0;
            int parts = 0;
            while (pos <= v.size() && parts < 4) {
                const size_t dot = v.find('.', pos);
                const std::string oct = v.substr(pos, dot == std::string::npos ? dot : dot - pos);
                if (oct.empty()) return false;
                acc = acc * 256 + std::stoi(oct);
                ++parts;
                if (dot == std::string::npos) break;
                pos = dot + 1;
            }
            if (parts != 4) return false;
            *out = acc;
            return true;
        }
        *out = big_int(v);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

CpTerm::Op parseOp(cstring s) {
    if (s == "eq") return CpTerm::Op::Eq;
    if (s == "neq") return CpTerm::Op::Neq;
    if (s == "in") return CpTerm::Op::In;
    if (s == "range") return CpTerm::Op::Range;
    if (s == "lpm") return CpTerm::Op::Lpm;
    if (s == "ternary") return CpTerm::Op::Ternary;
    return CpTerm::Op::Unsupported;  // never enforced - an unknown op must constrain nothing
}

/// One `when` term. A term whose op is unrecognized, or whose operands do not parse, stays
/// Unsupported so the clause degrades to documentation rather than to a wrong constraint.
CpTerm parseTerm(const JsonObject *o) {
    CpTerm t;
    if (o == nullptr) return t;
    t.key = strField(o, "key");
    t.op = parseOp(strField(o, "op"));
    // Record whether "value" was really there: consumers must not confuse a missing value with a
    // deliberate 0 (--dump-cp-stubs emits "value": null, which reads as absent here).
    t.hasValue = numField(o, "value", &t.value);
    numField(o, "mask", &t.mask);
    numField(o, "lo", &t.lo);
    numField(o, "hi", &t.hi);
    if (big_int p; numField(o, "prefix", &p)) t.prefix = static_cast<int>(p);
    if (const auto *vs = field(o, "values"); vs != nullptr) {
        if (const auto *arr = vs->to<JsonVector>(); arr != nullptr) {
            for (const auto &e : *arr) {
                if (const auto *n = e->to<JsonNumber>(); n != nullptr) {
                    t.values.emplace_back(static_cast<int64_t>(*n));
                }
            }
        }
    }
    if (const auto *ad = field(o, "action_data"); ad != nullptr) {
        if (const auto *arr = ad->to<JsonVector>(); arr != nullptr && arr->size() == 2) {
            const auto *a0 = (*arr)[0]->to<JsonString>();
            const auto *a1 = (*arr)[1]->to<JsonString>();
            if (a0 != nullptr && a1 != nullptr) {
                t.actionDataAction = cstring(a0->c_str());
                t.actionDataArg = cstring(a1->c_str());
            }
        }
    }
    if (const auto *rhs = field(o, "rhs"); rhs != nullptr) {
        if (const auto *ro = rhs->to<JsonObject>(); ro != nullptr) t.rhsVar = strField(ro, "var");
    }
    // A key term needs a key name; an action_data term needs its pair. Neither -> not enforceable.
    if (t.key.isNullOrEmpty() && t.actionDataArg.isNullOrEmpty()) t.op = CpTerm::Op::Unsupported;
    return t;
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
                // Only a scalar initial value is usable: a struct initialiser such as {1, 0} needs
                // field-wise seeding, which the register model does not expose here, so it is
                // recorded in the annotation but deliberately left unapplied.
                if (const auto *iv = field(ro, "initial_value"); iv != nullptr) {
                    if (const auto *ivo = iv->to<JsonObject>(); ivo != nullptr) {
                        if (big_int v; numField(ivo, "value", &v)) {
                            rule.hasInitialValue = true;
                            rule.initialValue = v;
                        }
                    }
                }
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
                // Structured when/then form takes precedence; `clause` remains for legacy files.
                CpAssumeClause c;
                if (const auto *w = field(o, "when"); w != nullptr || field(o, "then") != nullptr) {
                    c.kind = CpAssumeClause::Kind::WhenThen;
                    c.table = strField(o, "table");
                    if (const auto *arr = w != nullptr ? w->to<JsonVector>() : nullptr;
                        arr != nullptr) {
                        for (const auto &te : *arr) c.when.push_back(parseTerm(te->to<JsonObject>()));
                    }
                    if (const auto *th = field(o, "then"); th != nullptr) {
                        if (const auto *to = th->to<JsonObject>(); to != nullptr) {
                            c.thenAction = strField(to, "action");
                            c.thenActionNe = strField(to, "action_ne");
                        }
                    }
                    // No table, or nothing to assert about the action, means nothing to enforce.
                    if (c.table.isNullOrEmpty() ||
                        (c.thenAction.isNullOrEmpty() && c.thenActionNe.isNullOrEmpty())) {
                        c.kind = CpAssumeClause::Kind::Unparsed;
                    }
                } else {
                    c = parseClause(strField(o, "clause"));
                }
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

const CpAssumeClause *CpAnnotation::defaultActionClause(cstring table, cstring action) const {
    const auto tail = [](const std::string &s) {
        auto p = s.find_last_of('.');
        return p == std::string::npos ? s : s.substr(p + 1);
    };
    const std::string wantAction = tail(std::string(action.string_view()));
    for (const auto *c : clausesFor(table)) {
        if (c->kind != CpAssumeClause::Kind::DefaultAction) continue;
        if (c->action == action || tail(std::string(c->action.string_view())) == wantAction) {
            return c;
        }
    }
    return nullptr;
}

const CpAnnotation *loadedCpAnnotation() {
    static std::optional<CpAnnotation> cache;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const auto &path = SymbexOptions::get().cpAnnotationPath;
        if (path.has_value()) cache = CpAnnotation::load(*path);
    }
    return cache.has_value() ? &cache.value() : nullptr;
}

}  // namespace P4::P4Tools::Symbex
