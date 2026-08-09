#include "backends/p4tools/modules/symbex/core/symbolic_executor/sink_divergence.h"

#include <map>
#include <utility>
#include <vector>

#include "ir/ir.h"
#include "ir/visitor.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

namespace {

/// Replaces references to an action's parameters with their bound argument expressions, so two
/// calls of the same action with different action data yield structurally different bodies.
class ActionParamSubstitute : public Transform {
 public:
    explicit ActionParamSubstitute(std::map<cstring, const IR::Expression *> binding)
        : binding(std::move(binding)) {}
    const IR::Node *postorder(IR::PathExpression *pe) override {
        auto it = binding.find(pe->path->name.name);
        if (it != binding.end()) return it->second;
        return pe;
    }

 private:
    std::map<cstring, const IR::Expression *> binding;
};

bool exprEquiv(const IR::Expression *a, const IR::Expression *b) {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return a->equiv(*b);
}

/// Bind an action call's arguments to the callee's parameter names. Empty when the call and the
/// declaration disagree on arity, which leaves the body unsubstituted and therefore conservative.
std::map<cstring, const IR::Expression *> bindCallArgs(const IR::P4Action *action,
                                                       const IR::MethodCallExpression *mce) {
    std::map<cstring, const IR::Expression *> binding;
    if (action == nullptr || mce == nullptr) return binding;
    const auto &params = action->parameters->parameters;
    const auto *args = mce->arguments;
    if (args == nullptr || params.size() != args->size()) return binding;
    for (size_t i = 0; i < params.size(); ++i)
        binding[params.at(i)->name.name] = args->at(i)->expression;
    return binding;
}

}  // namespace

void collectEffect(const IR::Statement *stmt, ActionEffect &eff) {
    if (stmt == nullptr) return;
    if (const auto *block = stmt->to<IR::BlockStatement>()) {
        for (const auto *c : block->components) {
            const auto *s = c->to<IR::Statement>();
            if (s == nullptr) {  // a nested declaration we do not model
                eff.ok = false;
                return;
            }
            collectEffect(s, eff);
            if (!eff.ok) return;
        }
        return;
    }
    if (const auto *asg = stmt->to<IR::AssignmentStatement>()) {
        eff.assigns.emplace_back(asg->left, asg->right);
        return;
    }
    if (const auto *mc = stmt->to<IR::MethodCallStatement>()) {
        eff.calls.push_back(mc->methodCall);
        return;
    }
    if (stmt->is<IR::EmptyStatement>()) return;
    // if/switch/return/exit/...: cannot summarize statically -> be conservative.
    eff.ok = false;
}

bool effectsEqual(const ActionEffect &a, const ActionEffect &b) {
    if (!a.ok || !b.ok) return false;  // unsummarizable -> not provably equal
    if (a.assigns.size() != b.assigns.size() || a.calls.size() != b.calls.size()) return false;
    for (size_t i = 0; i < a.assigns.size(); ++i) {
        if (!exprEquiv(a.assigns[i].first, b.assigns[i].first)) return false;
        if (!exprEquiv(a.assigns[i].second, b.assigns[i].second)) return false;
    }
    for (size_t i = 0; i < a.calls.size(); ++i)
        if (!exprEquiv(a.calls[i], b.calls[i])) return false;
    return true;
}

ActionEffect summarizeAction(const IR::P4Action *action,
                             const std::map<cstring, const IR::Expression *> &binding) {
    ActionEffect eff;
    if (action == nullptr || action->body == nullptr) {
        eff.ok = false;
        return eff;
    }
    ActionParamSubstitute subst(binding);
    const auto *body = action->body->apply(subst)->to<IR::BlockStatement>();
    if (body == nullptr) {
        eff.ok = false;
        return eff;
    }
    collectEffect(body, eff);
    return eff;
}

bool outcomesDiverge(const IR::P4Action *actionA,
                     const std::map<cstring, const IR::Expression *> &bindingA,
                     const IR::P4Action *actionB,
                     const std::map<cstring, const IR::Expression *> &bindingB) {
    // Diverge unless the two bodies are provably identical in observable effect.
    return !effectsEqual(summarizeAction(actionA, bindingA), summarizeAction(actionB, bindingB));
}

bool branchesDiverge(const IR::IfStatement *cond) {
    if (cond == nullptr) return true;
    ActionEffect thenEff;
    ActionEffect elseEff;
    collectEffect(cond->ifTrue, thenEff);
    if (cond->ifFalse != nullptr) collectEffect(cond->ifFalse, elseEff);
    return !effectsEqual(thenEff, elseEff);
}

bool constEntryActionsDiverge(const IR::P4Table *table, const ActionResolver &resolve) {
    if (table == nullptr) return true;
    const auto *entries = table->getEntries();
    if (entries == nullptr || entries->entries.size() < 2) {
        // Fewer than two outcomes: moving the key cannot select a different one.
        return false;
    }
    // Compare every entry against the first. Any observable difference makes the map a real
    // selector; if all entries match the first, no key movement within it changes anything.
    const IR::P4Action *firstAction = nullptr;
    std::map<cstring, const IR::Expression *> firstBinding;
    for (const auto *entry : entries->entries) {
        const auto *mce = entry->action->to<IR::MethodCallExpression>();
        if (mce == nullptr) return true;  // unanalyzable -> assume observable
        const auto *path = mce->method->to<IR::PathExpression>();
        if (path == nullptr) return true;
        const auto *action = resolve ? resolve(path->path->name.name) : nullptr;
        if (action == nullptr) return true;
        auto binding = bindCallArgs(action, mce);
        if (firstAction == nullptr) {
            firstAction = action;
            firstBinding = std::move(binding);
            continue;
        }
        if (outcomesDiverge(firstAction, firstBinding, action, binding)) return true;
    }
    return false;
}

}  // namespace P4::P4Tools::Symbex
