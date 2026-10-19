#!/usr/bin/env python3
"""Write --cp-snapshot files for static-rule programs from their rule sources.

    write_static.py [<program> ...] [--out DIR]

Tofino programs come from static.py generators (packages.static_path; names checked against
bf-rt.json); bmv2 programs from simple_switch_CLI command files via cli2snap. These
hand-transcribed snapshots are also the reference a live capture of the same program must
reproduce.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot import packages, snapshot as snap  # noqa: E402
from cp_snapshot.bfrt_rules import BfrtRules  # noqa: E402

DEFAULT_OUT = Path("/root/jiwon/Workspace-remote/sp27/cp_snapshots")


def load_generator(program: str):
    return packages.load_module(packages.static_path(program), "static")


def write(program: str, out_dir: Path) -> Path:
    gen = load_generator(program)
    notes = [f"source: {s}" for s in getattr(gen, "SOURCES", [])]
    notes += [f"note: {n}" for n in getattr(gen, "NOTES", [])]
    if gen.TARGET == "tofino":
        rules = BfrtRules(program, getattr(gen, "BFRT_JSON", None),
                          schema_optional=getattr(gen, "SCHEMA_OPTIONAL", False))
        gen.build(rules)
        entities = rules.entities
    else:
        result = gen.build()
        entities = getattr(result, "entities", result)
        notes += [f"skipped (the switch rejects it): {l}" for l in getattr(result, "skipped", [])]
    s = snap.Snapshot(program=program, target=gen.TARGET, ports=gen.PORTS, entities=entities,
                      snapshot_id="static", notes=["hand-written from the program's static rules"]
                      + notes)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{program}.txtpb"
    path.write_text(snap.to_textproto(s))
    return path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("programs", nargs="*")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    programs = args.programs or packages.static_programs()
    for p in programs:
        path = write(p, args.out)
        n = sum(1 for l in path.read_text().splitlines() if l in ("entities {", "bfrt_entities {"))
        print(f"{p}: {n} entities -> {path}")


if __name__ == "__main__":
    main()
