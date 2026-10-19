#!/usr/bin/env python3
"""Compare a live capture with the hand-written (static) snapshot of the same program.

    compare.py <program> [--work DIR]

Reads <work>/entities.pkl from capture.py and rebuilds the static entities from
static/<program>.py, then normalizes representation only:
  - key-field order;
  - a ternary value under its mask (the switch stores value & mask; p4symbex matches
    (key & mask) == (value & mask), so both spellings are the same rule);
  - a leading '$' on fixed-table action names;
  - fixed-table ($pre/$mirror) fields the static snapshot leaves at their defaults.
Writes <work>/compare.txt and prints its first line.
"""
from __future__ import annotations

import argparse
import pickle
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot.capture import WORK  # noqa: E402
from cp_snapshot.write_static import load_generator  # noqa: E402


def _canon(k):
    if k.kind == "ternary" and isinstance(k.value, int) and isinstance(k.second, int):
        return type(k)(k.name, k.kind, k.value & k.second, k.second)
    return k


def ident(e):
    return (e.table, e.is_default, tuple(sorted((_canon(k) for k in e.keys), key=lambda k: k.name)),
            e.priority if e.keys else 0)


def static_entities(program: str):
    gen = load_generator(program)
    if getattr(gen, "TARGET", "tofino") == "bmv2":
        built = gen.build()
        return built if isinstance(built, list) else built.entities
    from cp_snapshot.bfrt_rules import BfrtRules
    rules = BfrtRules(program, getattr(gen, "BFRT_JSON", None),
                      schema_optional=getattr(gen, "SCHEMA_OPTIONAL", False))
    gen.build(rules)
    return rules.entities


def compare(program: str, work: Path) -> str:
    live = {ident(e): e for e in pickle.loads((work / "entities.pkl").read_bytes())["changed"]}
    manual = {ident(e): e for e in static_entities(program)}
    same, differ, extra = [], [], []
    for k in manual.keys() & live.keys():
        m, lv = manual[k], live[k]
        fixed = lv.table.startswith("$")
        laction = lv.action[1:] if fixed and lv.action.startswith("$") else lv.action
        ldata = dict(lv.data)
        if fixed:
            extra.append((k, sorted(set(ldata) - set(dict(m.data)))))
            ldata = {n: ldata.get(n) for n in dict(m.data)}
        if (m.action, dict(m.data)) == (laction, ldata):
            same.append(k)
        else:
            differ.append((m, lv))
    only_live = [live[k] for k in live.keys() - manual.keys()]
    only_manual = [manual[k] for k in manual.keys() - live.keys()]
    out = [f"{program}: static {len(manual)}  live {len(live)}  same {len(same)}  "
           f"differ {len(differ)}  only-live {len(only_live)}  only-static {len(only_manual)}"]
    out += [f"DIFFER  {m.table} {m.keys}\n  static: {m.action} {m.data}\n  live:   {lv.action} "
            f"{lv.data}" for m, lv in differ]
    out += [f"ONLY-LIVE    {e}" for e in only_live]
    out += [f"ONLY-STATIC  {e}" for e in only_manual]
    out += [f"(fixed table {k[0]} {k[2]}: live also lists default fields {f})"
            for k, f in extra if f]
    (work / "compare.txt").write_text("\n".join(out) + "\n")
    return out[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program")
    ap.add_argument("--work", type=Path)
    a = ap.parse_args()
    print(compare(a.program, (a.work or WORK / a.program).resolve()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
