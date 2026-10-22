#include "backends/p4tools/modules/symbex/lib/action_param_domains.h"

#include <map>
#include <optional>
#include <string>

#include "ir/visitor.h"

#include "backends/p4tools/modules/symbex/options.h"

namespace P4::P4Tools::Symbex {

namespace {

struct Domain {
    cstring actionName;
    cstring paramName;
    std::vector<big_int> values;
};

/// What collect() recorded for the program being executed.
std::vector<Domain> domains;

cstring nameTail(cstring name) {
    const std::string s(name.string_view());
    const auto dot = s.rfind('.');
    return dot == std::string::npos ? name : cstring(s.substr(dot + 1));
}

/// Gathers serializable enums (with their member values), typedefs, and the enum-typed parameters
/// of every action. Declarations are visited before the actions that use them, but the parameter
/// types are resolved only once the whole program has been seen, so the order does not matter.
class Collector : public Inspector {
 public:
    std::map<cstring, std::optional<std::vector<big_int>>> enums;
    std::map<cstring, const IR::Type *> typedefs;
    std::vector<std::pair<const IR::P4Action *, const IR::Parameter *>> params;

    bool preorder(const IR::Type_SerEnum *e) override {
        std::vector<big_int> values;
        for (const auto *member : e->members) {
            const auto *constant = member->value->to<IR::Constant>();
            if (constant == nullptr) {
                // A member the frontend did not fold to a literal: no reliable domain.
                enums[e->name.name] = std::nullopt;
                return false;
            }
            values.push_back(constant->value);
        }
        enums[e->name.name] = values;
        return false;
    }

    bool preorder(const IR::Type_Typedef *t) override {
        typedefs[t->name.name] = t->type;
        return false;
    }

    bool preorder(const IR::P4Action *action) override {
        for (const auto *param : action->parameters->parameters) {
            params.emplace_back(action, param);
        }
        return false;
    }

    /// The enum @p type names, following typedefs; nullptr when it is not a serializable enum.
    const std::vector<big_int> *enumOf(const IR::Type *type) const {
        for (int depth = 0; depth < 16 && type != nullptr; ++depth) {
            const auto *name = type->to<IR::Type_Name>();
            if (name == nullptr) return nullptr;
            const cstring n = name->path->name.name;
            if (auto it = enums.find(n); it != enums.end()) {
                return it->second.has_value() ? &*it->second : nullptr;
            }
            auto td = typedefs.find(n);
            type = td == typedefs.end() ? nullptr : td->second;
        }
        return nullptr;
    }
};

}  // namespace

void ActionParamDomains::collect(const IR::P4Program *program) {
    domains.clear();
    // Nothing reads the domains under --no-enum-action-domain, so do not even walk the program.
    if (program == nullptr || !SymbexOptions::get().enumActionDomain) return;
    Collector collector;
    program->apply(collector);
    for (const auto &[action, param] : collector.params) {
        if (const auto *values = collector.enumOf(param->type)) {
            domains.push_back({action->controlPlaneName(), param->controlPlaneName(), *values});
        }
    }
}

const std::vector<big_int> *ActionParamDomains::lookup(cstring actionName, cstring paramName) {
    for (const auto &d : domains) {
        if (d.paramName == paramName && d.actionName == actionName) return &d.values;
    }
    for (const auto &d : domains) {
        if (d.paramName == paramName && nameTail(d.actionName) == nameTail(actionName)) {
            return &d.values;
        }
    }
    return nullptr;
}

}  // namespace P4::P4Tools::Symbex
