#!/usr/bin/env python3
"""Replay a cp-snapshot campaign's tofino cases on tofino-model (phase-0 snapshot install, PORT
verdict), one program/category directory at a time.

    replay_campaign.py <campaign_dir> [program ...]

Each <campaign_dir>/<program>/<category>/*.txtpb is replayed by tofino_driver.do_testing with
managed tofino-model/bf_switchd processes; verdicts go to <campaign_dir>/replay.csv as
program,category,case,status,reason (OK = the attack reproduced as the case claims).
"""
from __future__ import annotations

import argparse
import csv
import logging
import sys
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot import capture  # noqa: E402,F401  (sets the SDE environment)
from tofino import tofino_driver as td  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("campaign", type=Path)
    ap.add_argument("programs", nargs="*")
    ap.add_argument("--phase-timeout", type=float, default=2.0)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    camp = a.campaign.resolve()
    rows = []
    for pdir in sorted(d for d in camp.iterdir() if d.is_dir()):
        if a.programs and pdir.name not in a.programs:
            continue
        for cdir in sorted(d for d in pdir.iterdir() if d.is_dir()):
            cases = sorted(cdir.glob("*.txtpb"))
            snap = (capture.SNAPS / "live" / f"{pdir.name}.txtpb")
            if not cases or "target: \"tofino\"" not in snap.read_text()[:2000]:
                continue
            spec = SimpleNamespace(name=pdir.name, p4_name=pdir.name)
            args = SimpleNamespace(output_root=str(camp / "_replay" / cdir.name), arch="tna",
                                   phase_timeout=a.phase_timeout, reset_mode="restart",
                                   strong_verify=False)

            def writer(prog, case, status, reason, _cat=cdir.name):
                rows.append((prog, _cat, case, status, reason))

            td.do_testing(spec, Path(), cases, args, writer, lambda *x: None, manage_procs=True)
    out = camp / "replay.csv"
    with out.open("a", newline="") as fd:
        csv.writer(fd).writerows(rows)
    for r in rows:
        print(",".join(map(str, r)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
