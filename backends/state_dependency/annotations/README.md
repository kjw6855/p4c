# Corpus cp-annotation copies

These are **copies** of corpus `--cp-annotation` files that this repo's changes depend on. The live
files the nightly actually reads live outside p4c, in
`~/Workspace-remote/p4symbex_nightly/annotations/`, which is not a git repo — so a change there is
otherwise unrecoverable and invisible in review. The copies exist to make the change reviewable and
restorable; they are not loaded by anything at build or test time.

## What was added, and why

All three files gained one `assume` clause pinning a **control-plane threshold** that the sketch
compares its counter against:

| file | table | action | arg | value |
|---|---|---|---|---|
| `SketchLib_..._countmin_...` | `SwitchIngress.get_threshold.tbl_get_threshold` | `tbl_get_threshold_act` | `threshold` | 30 |
| `SketchLib_..._countsketch_...` | `SwitchIngress.get_threshold.tbl_get_threshold` | `tbl_get_threshold_act` | `threshold` | 30 |
| `SketchLib_..._univmon_...` | `SwitchIngress.tcam.tbl_select_level` | `tbl_act` | `threshold` | 30 |

Table, action and argument names are the qualified names p4symbex itself emits in the generated
`txtpb` (`table_name:` fields), not names read off the P4 source — the tables sit inside nested
control instances (`GET_THRESHOLD() get_threshold`, `lpm_optimization() tcam`), so the qualified
names are not guessable from the source.

Without the clause, the synthesized `threshold` action argument is free-symbolic: the solver picks
0, every sink flips on the first packet, and the emitted tests are single-send false positives. The
sketch counter is compared against this value *outside* the RegisterAction
(`est = est - threshold`, sink keyed on the sign bit), so no constant relation exists in the write
path for `driveRegisterPhase2` to solve a packet count against.

Value **30** is a stated project assumption with `"source": "controller-assumption"` — SketchLib
ships only `README.md` and `compile.sh`, no controller and no entry dump, so no `cp-artifact` can be
cited. It is deliberately far below a realistic threshold because `k` is replayed as real packets
and a 10000-packet flood is impractical.

## Restoring

The pre-change originals are at
`~/Workspace-remote/p4symbex_nightly/annotations_pre_step2c/`. The generator is idempotent and
re-runnable; it skips a file that already carries the clause.

## Caveat

These files are shared with the Tofino VM over the `Workspace-remote` mount, and the nightly sweep
reads them. Changing them changes nightly generation for these three programs — that is intended
here, but it means the nightly's countmin numbers are not comparable across this change.
