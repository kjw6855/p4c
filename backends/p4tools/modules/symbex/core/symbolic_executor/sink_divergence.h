#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_SINK_DIVERGENCE_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_SINK_DIVERGENCE_H_

#include <map>
#include <utility>
#include <vector>

#include "ir/ir.h"
#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// Does flipping a sink from one outcome to another change anything an observer could see?
///
/// A tampering test is only worth emitting when the answer is yes. All three sink kinds ask exactly
/// this question, differing only in which pair of program fragments is compared:
///
///   | sink kind                      | pair compared                              |
///   |--------------------------------|--------------------------------------------|
///   | h2s2k, control-plane table     | candidate action vs `default_action`       |
///   | h2s2k, const-entry table       | action_i vs action_j from the const map    |
///   | h2s2c, condition               | then-branch vs else-branch                 |
///
/// This module holds the one comparison all three share. It was previously three file-local statics
/// inside state_dependency_track.cpp, reachable only from that translation unit.
///
/// Every predicate here is **sound toward emitting**: anything it cannot summarize (control flow in
/// an action body, an unresolvable action, a nested declaration) reports "diverges", so an
/// unanalyzable sink produces a test rather than silently dropping a real finding.

/// The output-affecting effect of an action body or branch, after action parameters have been
/// substituted with their bound arguments. `ok = false` marks a body that cannot be summarized;
/// callers must then treat the two sides as divergent.
struct ActionEffect {
    bool ok = true;
    std::vector<std::pair<const IR::Expression *, const IR::Expression *>> assigns;  // (lhs, rhs)
    std::vector<const IR::Expression *> calls;  // method/extern calls (e.g. mark_to_drop)
};

/// Accumulate @p stmt's effect into @p eff. Sets `eff.ok = false` on anything unsummarizable.
void collectEffect(const IR::Statement *stmt, ActionEffect &eff);

/// True when two effects are provably identical. Two unsummarizable effects are NOT equal.
bool effectsEqual(const ActionEffect &a, const ActionEffect &b);

/// Effect of @p action with its parameters replaced by @p binding, so two calls of the same action
/// with different action data compare as different.
ActionEffect summarizeAction(const IR::P4Action *action,
                             const std::map<cstring, const IR::Expression *> &binding);

/// The shared entry point: do these two actions differ observably under their respective bindings?
bool outcomesDiverge(const IR::P4Action *actionA,
                     const std::map<cstring, const IR::Expression *> &bindingA,
                     const IR::P4Action *actionB,
                     const std::map<cstring, const IR::Expression *> &bindingB);

/// Condition-sink form: do @p cond's then- and else-branches differ observably? A null else-branch
/// contributes an empty effect, which differs from any non-empty then-branch.
bool branchesDiverge(const IR::IfStatement *cond);

/// Const-entry-sink form: do any two entries of @p table select observably different actions?
///
/// For a table whose key->action map is fixed in the program, HIT-vs-MISS is the wrong question --
/// it cannot miss (see constEntriesCoverKeySpace) -- but moving the key from one const entry to
/// another is a real, observable change whenever the two entries name actions that differ. False
/// when every entry has the same observable effect, i.e. the const map is a no-op selector and no
/// register value flowing into it can change anything.
bool constEntryActionsDiverge(const IR::P4Table *table);

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_CORE_SYMBOLIC_EXECUTOR_SINK_DIVERGENCE_H_ */
