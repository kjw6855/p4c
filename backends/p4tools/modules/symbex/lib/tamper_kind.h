#ifndef BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TAMPER_KIND_H_
#define BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TAMPER_KIND_H_

#include <cstdint>

#include "lib/cstring.h"

namespace P4::P4Tools::Symbex {

/// What a tampering test case claims the attacker changed at the sink.
///
/// This replaces the `bool missToHit` the generator and every emitter used to pass around. A bool
/// can only name the two directions of a HIT/MISS flip, but the replay oracle compares two Phase-3
/// outputs and never asks about hit/miss, so a sink can equally diverge by running a DIFFERENT
/// ACTION in both runs — the case of a const-entries table that is total over its key bits and can
/// never MISS. The two condition cases were already outside the bool's vocabulary: they were
/// squeezed into it by reusing false/true for TRUE→FALSE/FALSE→TRUE and carried honestly only
/// out-of-band, in `caseLabel`.
///
/// Kept in its own header because the generator (state_dependency_track.h) and the abstract test
/// spec (lib/test_spec.h) both need it and neither includes the other.
///
/// Not every enumerator is selected yet: the generator currently emits only HitToMiss and MissToHit,
/// condition sinks included. Moving condition sinks onto the Cond* kinds would rename their output
/// directories, so it is an output change that belongs in its own commit rather than in the refactor
/// that introduced this enum.
enum class TamperKind : uint8_t {
    /// Sink table HITs in the legit run and MISSes once the attacker's write is replayed.
    HitToMiss,
    /// Sink table MISSes in the legit run and HITs once the attacker's write is replayed.
    MissToHit,
    /// Sink table is reached in both runs but runs an observably different action, or the same
    /// action under different action data. Neither `hit_phase` nor `miss_phase` can describe this.
    ActionDiverge,
    /// Condition sink: the then-branch runs in the legit run, the else-branch after the write.
    CondTrueToFalse,
    /// Condition sink: the else-branch runs in the legit run, the then-branch after the write.
    CondFalseToTrue,
};

/// The short tag naming @p kind in emitted file names, PTF class names and report metadata. One
/// definition on purpose: the tag used to be spelled as a separate ternary in each of the four test
/// back ends, so a file name and the metadata inside that file could drift apart.
inline cstring tamperKindTag(TamperKind kind) {
    switch (kind) {
        case TamperKind::HitToMiss:
            return "h2m"_cs;
        case TamperKind::MissToHit:
            return "m2h"_cs;
        case TamperKind::ActionDiverge:
            return "adiv"_cs;
        case TamperKind::CondTrueToFalse:
            return "c_t2f"_cs;
        case TamperKind::CondFalseToTrue:
            return "c_f2t"_cs;
    }
    // Unreachable for a well-formed enumerator; a value out of range would otherwise fall off the
    // end of a non-void function.
    return "h2m"_cs;
}

}  // namespace P4::P4Tools::Symbex

#endif /* BACKENDS_P4TOOLS_MODULES_SYMBEX_LIB_TAMPER_KIND_H_ */
