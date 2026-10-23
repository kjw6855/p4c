#!/usr/bin/env python3
"""Which A2S chains does a snapshot sequence's controller steps reach?

    a2s_sources.py <sequence.txtpb> [--chains FILE] [--p4info FILE] [--csv FILE]

An A2S chain (a2s2o / a2s2outk / a2s2outc in the program's chain cache) names the control-plane
sources of its stateful object in its `cp` tag: writeData (action params whose value a packet
stores: a CP update leaves the stored value stale, "inconsistency") and writeIdx / readIdx (action
params that pick the cell: a CP update moves the cell, "residual"). A step covers a chain when one
of its updates
  - inserts or deletes an entry of a source table whose action is the source action,
  - modifies such an entry so that the source param's value or the action changes, or
  - writes a cell of the chain's stateful object (register; counts for every kind the chain has).
Unless some step covers a chain, no A2S case can come out of that scenario for it. p4symbex
--state-dep-source A2S applies the same rules (a2s_relevance.csv has the same columns).
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot import packages, snapshot as snap  # noqa: E402
from cp_snapshot.snapshot import Entity  # noqa: E402

CACHE = Path("/root/jiwon/Workspace-remote/sp27/p4symbex_nightly/sochain_cache")
CATEGORIES = ("a2s2o", "a2s2outk", "a2s2outc")
COLUMNS = ["chain_id", "category", "kind", "step", "update_index", "table", "action", "param",
           "reason"]


def name_match(a: str, b: str) -> bool:
    """Same object under two spellings: equal, or one is a dotted suffix of the other (BfRt adds
    the pipeline, "pipe.", and P4Info may add the control)."""
    return bool(a) and bool(b) and (a == b or a.endswith("." + b) or b.endswith("." + a))


def touches(op: str, new: Entity, old: Optional[Entity], src: dict) -> Optional[str]:
    """Why update (op, new) of an entry that was `old` before the step changes CP source `src`
    ({tables, action, param}), or None."""
    if not any(name_match(new.table, t) for t in src.get("tables", [])):
        return None
    acts = [e.action for e in (new, old) if e is not None and e.action]
    if not any(name_match(a, src["action"]) for a in acts):
        return None
    if op != "MODIFY" or old is None:
        return f"{op} of a {src['action']} entry"
    if old.action != new.action:
        return f"action {old.action} -> {new.action}"
    before, after = dict(old.data).get(src["param"]), dict(new.data).get(src["param"])
    if before != after:
        return f"{src['param']} {before} -> {after}"
    return None


def chain_kinds(cp: dict) -> List[str]:
    kinds = []
    if cp.get("writeData"):
        kinds.append("inconsistency")
    if cp.get("writeIdx") and cp.get("readIdx"):
        kinds.append("residual")
    return kinds


def register_name(e: Entity, p4info: Optional[dict]) -> str:
    if e.table.startswith("register_id:") and p4info:
        rid = int(e.table.split(":", 1)[1])
        for r in p4info.get("registers", []):
            if r["preamble"]["id"] == rid:
                return r["preamble"]["name"]
    return e.table


def coverage(seq: snap.Sequence, chains: dict, p4info: Optional[dict] = None) -> List[dict]:
    """One row per (chain, kind, covering update); chains no step covers get no row."""
    rows: List[dict] = []
    for j, st in enumerate(seq.steps, 1):
        prev = {e.ident(): e for e in snap.materialize(seq, j - 1).entities}
        for i, (op, e) in enumerate(st.updates):
            old = prev.get(e.ident())
            for cat in CATEGORIES:
                for ctl, chs in chains.get(cat, {}).items():
                    for ch in chs:
                        cp = ch.get("cp") or {}
                        if e.is_register:
                            if name_match(register_name(e, p4info), ch.get("soName", "")):
                                for kind in chain_kinds(cp):
                                    rows.append(_row(ch, cat, kind, st, i, e, "", "", "register "
                                                     f"cell of {ch['soName']}"))
                            continue
                        for kind, srcs in (("inconsistency", cp.get("writeData", [])),
                                           ("residual", cp.get("writeIdx", []) +
                                            cp.get("readIdx", []))):
                            if kind not in chain_kinds(cp):
                                continue
                            for src in srcs:
                                why = touches(op, e, old, src)
                                if why:
                                    rows.append(_row(ch, cat, kind, st, i, e, src["action"],
                                                     src["param"], why))
    return rows


def _row(ch, cat, kind, st, i, e, action, param, reason) -> dict:
    return {"chain_id": ch["id"], "category": cat, "kind": kind, "step": st.step_id,
            "update_index": i, "table": e.table, "action": action, "param": param,
            "reason": reason}


def all_chains(chains: dict) -> Iterable[Tuple[str, dict]]:
    for cat in CATEGORIES:
        for ctl, chs in chains.get(cat, {}).items():
            for ch in chs:
                yield cat, ch


def default_chains(program: str) -> Path:
    pkg = packages.package_dir(program)
    if pkg is not None:
        return CACHE / pkg.parent.parent.name / pkg.parent.name / f"{program}.chains"
    # A package named apart from its program (scenario PROGRAM): find the program in the cache.
    found = sorted(CACHE.glob(f"*/*/{program}.chains"))
    if not found:
        raise SystemExit(f"no package or cached chains for {program}; pass --chains")
    return found[0]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sequence", type=Path)
    ap.add_argument("--chains", type=Path, help="chain cache (default: the program's nightly one)")
    ap.add_argument("--p4info", type=Path, help="P4Info JSON naming bmv2 register ids")
    ap.add_argument("--csv", type=Path, help="write the covering updates here")
    a = ap.parse_args()
    seq = snap.parse_sequence(a.sequence.read_text())
    chains = json.loads((a.chains or default_chains(seq.init.program)).read_text())
    p4info = json.loads(a.p4info.read_text()) if a.p4info else None
    rows = coverage(seq, chains, p4info)
    covered: Dict[Tuple[str, int], List[dict]] = {}
    for r in rows:
        covered.setdefault((r["category"], r["chain_id"]), []).append(r)
    n = 0
    for cat, ch in all_chains(chains):
        hits = covered.get((cat, ch["id"]), [])
        kinds = "+".join(chain_kinds(ch.get("cp") or {})) or "-"
        if hits:
            n += 1
            first = hits[0]
            print(f"{cat} {ch['id']} ({kinds}, {ch['soName']}): covered by step {first['step']} "
                  f"update {first['update_index']} ({first['kind']}: {first['reason']})"
                  + (f" and {len(hits) - 1} more" if len(hits) > 1 else ""))
        else:
            print(f"{cat} {ch['id']} ({kinds}, {ch['soName']}): not covered")
    total = sum(1 for _ in all_chains(chains))
    print(f"covered={n}/{total} chains, {len(seq.steps)} steps, "
          f"{sum(len(s.updates) for s in seq.steps)} updates")
    if a.csv:
        with a.csv.open("w", newline="") as fd:
            w = csv.DictWriter(fd, COLUMNS)
            w.writeheader()
            w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
