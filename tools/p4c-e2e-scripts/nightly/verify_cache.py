#!/usr/bin/env python3
"""Verify that p4symbex emits IDENTICAL txtpb with and without the SOChain cache.

For the selected programs it runs `tampering.py --stage gen` twice — once normally (recomputing the
state-dependency analysis) and once with --state-dep-cache-root (loading the pre-built cache) — then
diffs the two txtpb trees, ignoring volatile lines (date / seed / coverage / the output-dir path that
p4symbex bakes into a comment). Any difference is a cache-correctness bug.

Requires the cache to already exist under --cache-root (build it first with cache_chains.py).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent  # p4c-e2e-scripts/

# Lines that legitimately differ run-to-run (or encode the output dir) and are excluded from the diff.
VOLATILE = ("Date generated", "symbex seed", "Current node coverage",
            "three-phase tampering test case for")


def _stripped(path: Path) -> list:
    return [ln for ln in path.read_text(errors="replace").splitlines()
            if not any(v in ln for v in VOLATILE)]


def _gen(outroot: Path, cache_root, args) -> None:
    cmd = [sys.executable, str(ROOT / "tampering.py"),
           "--target", args.target, "--arch", args.arch, "--stage", "gen",
           "--tamper-mode", args.tamper_mode, "--program-list", args.program_list,
           "--filter", args.filter, "--max-tests", str(args.max_tests),
           "--jobs", str(args.jobs), "--no-ui", "--output-root", str(outroot)]
    if cache_root:
        cmd += ["--state-dep-cache-root", str(cache_root)]
    # tofino sub-runs expect cwd=tofino/ (sibling-relative imports); bmv2 runs from the repo root.
    cwd = str(ROOT / "tofino") if args.target == "tofino" else str(ROOT)
    subprocess.run(cmd, cwd=cwd, check=False)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", default="tofino")
    ap.add_argument("--arch", default="tna")
    ap.add_argument("--tamper-mode", default="both")
    ap.add_argument("--program-list", required=True)
    ap.add_argument("--filter", required=True, help="substring match selecting the programs to check")
    ap.add_argument("--max-tests", type=int, default=4)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--cache-root", required=True, help="prebuilt <...>/sochain_cache directory")
    args = ap.parse_args()

    tmp = Path(tempfile.mkdtemp(prefix="verify_cache_"))
    a, b = tmp / "nocache", tmp / "cache"
    print(f"[verify] gen WITHOUT cache -> {a}", flush=True)
    _gen(a, None, args)
    print(f"[verify] gen WITH cache    -> {b}", flush=True)
    _gen(b, args.cache_root, args)

    af = {p.relative_to(a): p for p in a.rglob("*.txtpb")}
    bf = {p.relative_to(b): p for p in b.rglob("*.txtpb")}
    only_a = sorted(set(af) - set(bf))
    only_b = sorted(set(bf) - set(af))
    diffs = [rel for rel in sorted(set(af) & set(bf)) if _stripped(af[rel]) != _stripped(bf[rel])]

    print(f"[verify] nocache={len(af)} cache={len(bf)} only_nocache={len(only_a)} "
          f"only_cache={len(only_b)} content_diffs={len(diffs)}")
    for rel in only_a:
        print("  MISSING in cache:", rel)
    for rel in only_b:
        print("  EXTRA in cache:", rel)
    for rel in diffs:
        print("  DIFFERS:", rel)
    ok = not (only_a or only_b or diffs) and len(af) > 0
    print("[verify] RESULT:", "IDENTICAL" if ok else "MISMATCH")
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
