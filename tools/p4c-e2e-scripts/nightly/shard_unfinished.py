#!/usr/bin/env python3
"""Split the UNFINISHED work of a sweep across N machines.

A sweep leaves two kinds of hole behind, and they need different handling:

  timeouts       p4symbex was killed by the per-program `symbex_timeout=` budget. The failure is
                 PER POLICY -- builders.py deliberately keeps the surviving policy's output -- so
                 the unit of unfinished work is a (program, policy) pair, not a program.
  never-reached  the whole matrix cell hit its `timeout 6h` wrapper before the program was tried,
                 so neither policy ran and there is no output directory at all.

Given --shards N and --index I this writes the shard's share as ready-to-run program lists, one per
policy, copied VERBATIM from the source list so paths, arch and tokens are preserved.

Balancing is by BUDGET, not by count: effective budgets span 1800-10800s, so a naive `i % N` split
hands one machine every expensive program. Longest-processing-time bin packing (sort descending,
assign to the least-loaded shard) keeps the machines within a few percent of each other.

The split is deterministic -- same inputs, same shards on every machine -- so the only thing the
machines must agree on is N. Nothing here coordinates or locks.

Example:
    ./shard_unfinished.py --sweep-root ~/Workspace-remote/p4symbex_nightly_h2s2k \
                          --shards 4 --index 0 --print-cmd
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

# Corpus checkout (.p4 sources + *_program_list.txt). Env-overridable so the tree can live
# under a different root; matches SWEEP_TOP_TIER_REPO in nightly_sweep.sh.
TR = Path(os.environ.get("SWEEP_TOP_TIER_REPO",
                         "/home/vagrant/Workspace-remote/top_tier_repo"))

# builders.py logs a killed policy as ONE line:
#   [<program>] p4symbex policy <POLICY> failed: Command '[...]' timed out after <N> seconds
#
# Matched per line, and deliberately NOT with re.S + a lazy `.*?timed out`: policies also fail for
# reasons that are not timeouts (9 of 25 such lines in one real sweep), and a dot-matches-newline
# pattern happily runs from a NON-timeout failure forward into the next line that does say "timed
# out". finditer then consumes the real record, so the count comes out right while the program names
# are wrong -- which is exactly the kind of error a retry would act on silently.
_FAILED_RE = re.compile(
    r"\[(?P<prog>[A-Za-z0-9_.\-]+)\] p4symbex policy (?P<policy>\S+) failed:")

_POLICY_TO_MODE = {"STATE_DEP_TAMPERING": "key", "STATE_DEP_TAMPERING_COND": "cond"}

Pair = Tuple[str, str]          # (program, mode)


def parse_rows(list_path: Path) -> Dict[str, str]:
    """program name -> its verbatim program-list row."""
    rows: Dict[str, str] = {}
    for line in list_path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        rows[line.split()[0]] = line
    return rows


def row_budget(row: str) -> int:
    """The row's symbex_timeout= budget; 1800 when absent (tampering.py's default)."""
    for tok in row.split():
        if tok.startswith("symbex_timeout="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return 1800
    return 1800


def discover(sweep_root: Path, cell: str, rows: Dict[str, str], include: str,
             log_path: "Path | None" = None) -> Set[Pair]:
    """The unfinished (program, mode) pairs for this sweep."""
    pairs: Set[Pair] = set()
    log = log_path or sweep_root / "current" / "sweep.log"
    if include in ("timeouts", "all"):
        if not log.exists():
            sys.exit(f"no sweep log at {log} — nothing to discover")
        for line in log.read_text(errors="replace").splitlines():
            m = _FAILED_RE.search(line)
            if not m or "timed out" not in line:
                continue
            mode = _POLICY_TO_MODE.get(m.group("policy"))
            # Only programs still in the list can be re-run; a stale log may name others.
            if mode and m.group("prog") in rows:
                pairs.add((m.group("prog"), mode))
    if include in ("never-reached", "all"):
        cell_dir = sweep_root / "current" / cell
        attempted = {d.name for d in cell_dir.iterdir() if d.is_dir()} if cell_dir.is_dir() else set()
        for prog in rows:
            if prog not in attempted:
                pairs.update({(prog, "key"), (prog, "cond")})
    return pairs


def balance(pairs: Set[Pair], rows: Dict[str, str], floor: int,
            shards: int) -> List[List[Pair]]:
    """Longest-processing-time bin packing over the effective budgets."""
    def cost(p: Pair) -> int:
        return max(row_budget(rows[p[0]]), floor)

    # Sort by cost descending, then by name so ties break identically on every machine.
    ordered = sorted(pairs, key=lambda p: (-cost(p), p[0], p[1]))
    buckets: List[List[Pair]] = [[] for _ in range(shards)]
    loads = [0] * shards
    for p in ordered:
        i = loads.index(min(loads))
        buckets[i].append(p)
        loads[i] += cost(p)
    return buckets


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sweep-root", required=True, type=Path,
                    help="e.g. /home/vagrant/Workspace-remote/p4symbex_nightly_h2s2k")
    ap.add_argument("--shards", required=True, type=int, help="number of machines")
    ap.add_argument("--index", required=True, type=int, help="this machine, 0-based")
    ap.add_argument("--one-based", action="store_true", help="treat --index as 1-based")
    ap.add_argument("--list", type=Path, default=None,
                    help="source program list (default: <top_tier_repo>/<pol>_program_list.txt)")
    ap.add_argument("--cell", default=None, help="matrix cell dir (default tofino_tna_<pol>)")
    ap.add_argument("--log", type=Path, default=None,
                    help="sweep log to read (default <sweep-root>/current/sweep.log). A combined "
                         "root keeps one log per policy, e.g. current/sweep_h2s2c.log.")
    ap.add_argument("--policy", choices=("h2s2k", "h2s2c"), default=None,
                    help="which policy's list/cell to work on. Required when the sweep root holds "
                         "both (a single-machine run), inferred from a _h2s2k/_h2s2c suffix otherwise.")
    ap.add_argument("--include", choices=("timeouts", "never-reached", "all"), default="timeouts")
    ap.add_argument("--timeout-min", type=int, default=10800,
                    help="budget floor used ONLY for balancing; tokens are never rewritten "
                         "(pass the same value to tampering.py --symbex-timeout-min)")
    ap.add_argument("--out", type=Path, default=None, help="default <sweep-root>/retry")
    ap.add_argument("--annotations", type=Path, default=None,
                    help="cp-annotation root for --print-cmd (default <sweep-root>/annotations)")
    ap.add_argument("--print-cmd", action="store_true",
                    help="also print this shard's tampering.py invocations")
    args = ap.parse_args()

    idx = args.index - 1 if args.one_based else args.index
    if args.shards < 1:
        sys.exit("--shards must be >= 1")
    if not 0 <= idx < args.shards:
        sys.exit(f"--index {args.index} out of range for --shards {args.shards}")

    # Policy ("h2s2k"/"h2s2c") names the list, the cell and the output files. Inferred from a
    # _h2s2k/_h2s2c suffix when the sweep was split per policy across machines; on a single machine
    # both policies live in ONE root (the six-cell matrix writes one cell each), so there is no
    # suffix to read and --policy says which one to work on.
    pol = args.policy or args.sweep_root.name.rsplit("_", 1)[-1]
    if pol not in ("h2s2k", "h2s2c"):
        sys.exit(f"cannot infer policy from {args.sweep_root.name!r}; "
                 f"pass --policy h2s2k|h2s2c (a combined root has no policy suffix)")
    list_path = args.list or TR / f"{pol}_program_list.txt"
    cell = args.cell or f"tofino_tna_{pol}"
    out_dir = args.out or args.sweep_root / "retry"

    if not list_path.exists():
        sys.exit(f"program list not found: {list_path}")
    rows = parse_rows(list_path)
    pairs = discover(args.sweep_root, cell, rows, args.include, args.log)
    if not pairs:
        print(f"[{pol}] nothing unfinished for --include {args.include}")
        return 0

    buckets = balance(pairs, rows, args.timeout_min, args.shards)
    mine = buckets[idx]

    def hours(bs: List[Pair]) -> float:
        return sum(max(row_budget(rows[p[0]]), args.timeout_min) for p in bs) / 3600.0

    loads = [hours(b) for b in buckets]
    spread = (max(loads) - min(loads)) / max(loads) * 100 if max(loads) else 0.0
    print(f"[{pol}] {len(pairs)} unfinished (program, policy) pairs from {list_path.name}")
    print(f"[{pol}] {args.shards} shards, serial worst case {sum(loads):.1f}h, "
          f"per-shard {min(loads):.1f}-{max(loads):.1f}h (spread {spread:.0f}%)")

    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for mode in ("key", "cond"):
        progs = sorted({p for p, m in mine if m == mode})
        path = out_dir / f"shard{idx}of{args.shards}_{mode}.txt"
        if not progs:
            # Leave no stale list behind from an earlier split with different N.
            path.unlink(missing_ok=True)
            continue
        path.write_text("".join(rows[p] + "\n" for p in progs))
        written.append((mode, path, progs))
        print(f"[{pol}] shard {idx}: {len(progs):2d} {mode:4s} program(s) -> {path}")

    if not written:
        print(f"[{pol}] shard {idx} has no work")
    elif args.print_cmd:
        cell_dir = args.sweep_root / "current" / cell
        # Annotations sit beside the cache in whichever tree this sweep uses; do not assume
        # the canonical p4symbex_nightly path, which is wrong for a per-user or split root.
        ann_root = args.annotations or args.sweep_root / "annotations"
        nightly = Path(__file__).resolve().parent
        print()
        for mode, path, _ in written:
            print(f"python3 {nightly.parent}/tampering.py \\\n"
                  f"    --target tofino --arch tna --stage gen --tamper-mode {mode} \\\n"
                  f"    --program-list {path} --output-root {cell_dir} \\\n"
                  f"    --state-dep-cache-root {args.sweep_root}/sochain_cache \\\n"
                  f"    --cp-annotation-root {ann_root} \\\n"
                  f"    --shared-traversal PHASE1_PHASE2 --chain-impact-order \\\n"
                  f"    --max-tests 4 --jobs 4 --no-ui \\\n"
                  f"    --symbex-timeout-min {args.timeout_min} \\\n"
                  f"    2>&1 | tee {cell_dir}/retry_shard{idx}_{mode}.log")
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
