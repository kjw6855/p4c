#!/usr/bin/env python3
"""Step 2c: add the control-plane threshold `assume` clause to the three SketchLib annotations.

SketchLib ships only README.md + compile.sh -- no controller, no entries, no threshold value -- so
no `cp-artifact` can be cited. The value is a stated project assumption, deliberately low (30, not a
realistic 10000) because k is replayed as real packets and a 10000-packet flood is impractical.

Table/action/arg names are the qualified names p4symbex itself emits in the generated txtpb
(`table_name:` fields), not guesses from the P4 source.
"""
import json
import shutil
import sys
from pathlib import Path

ANN = Path.home() / "Workspace-remote/p4symbex_nightly/annotations"
BACKUP = Path.home() / "Workspace-remote/p4symbex_nightly/annotations_pre_step2c"
MIRROR = Path.home() / "Workspace/p4c/backends/state_dependency/annotations"

THRESHOLD = 30

TARGETS = {
    "SketchLib_p4_16_countmin_p416_countmin": (
        "SwitchIngress.get_threshold.tbl_get_threshold", "tbl_get_threshold_act", "threshold"),
    "SketchLib_p4_16_countsketch_p416_countsketch": (
        "SwitchIngress.get_threshold.tbl_get_threshold", "tbl_get_threshold_act", "threshold"),
    "SketchLib_p4_16_univmon_p416_univmon": (
        "SwitchIngress.tcam.tbl_select_level", "tbl_act", "threshold"),
}

REASON = (
    "SketchLib ships only README.md and compile.sh -- no controller, no entry dump, no threshold "
    "value -- so no cp-artifact can be cited. The sketch counter is compared against this "
    "control-plane action parameter OUTSIDE the RegisterAction (est = est - threshold, sink keyed "
    "on the sign bit), so without pinning it the argument stays free-symbolic and no packet count "
    "can be derived. Value 30 is deliberately low: k is replayed as real packets, so a realistic "
    "threshold would demand an impractical flood."
)

def main() -> int:
    BACKUP.mkdir(parents=True, exist_ok=True)
    MIRROR.mkdir(parents=True, exist_ok=True)
    for stem, (table, action, arg) in TARGETS.items():
        path = ANN / f"{stem}.json"
        if not path.is_file():
            print(f"FAIL: missing {path}")
            return 1
        data = json.loads(path.read_text())
        assume = data.setdefault("assume", [])
        # Idempotent: re-running must not stack duplicate clauses.
        if any(isinstance(c, dict) and c.get("table") == table and
               any(t.get("action_data") == [action, arg] for t in c.get("when", []))
               for c in assume):
            print(f"SKIP  {stem}: clause already present")
            continue
        if not (BACKUP / f"{stem}.json").exists():
            shutil.copy2(path, BACKUP / f"{stem}.json")
        assume.append({
            "table": table,
            # op is "eq" -- the loader's parseOp does NOT understand "==", and an unrecognized op
            # silently degrades the term to Unsupported, which enforces nothing.
            "when": [{"action_data": [action, arg], "op": "eq", "value": THRESHOLD}],
            # A WhenThen clause without a `then.action` is downgraded to Unparsed by the loader and
            # then skipped by clausesFor. This table has exactly one action, so naming it is both
            # required and trivially true.
            "then": {"action": action},
            "source": "controller-assumption",
            "ref": f"{stem}: no controller in the SketchLib artifact; threshold assumed",
            "reason": REASON,
        })
        path.write_text(json.dumps(data, indent=2) + "\n")
        shutil.copy2(path, MIRROR / f"{stem}.json")
        print(f"OK    {stem}: pinned {action}({arg}) = {THRESHOLD} on {table}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
