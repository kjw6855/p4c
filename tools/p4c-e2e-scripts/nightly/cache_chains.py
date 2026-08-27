#!/usr/bin/env python3
"""Pre-compute and cache state-dependency SOChains for every program in the sweep matrix.

The IFDS state-dependency analysis dominates p4symbex runtime on large programs (netlock ~minutes)
and is otherwise recomputed on every p4symbex invocation — including once per tamper-mode policy.
This script runs the analysis ONCE per (target, arch, program); the resulting JSON cache holds BOTH
Key (h2s2k) and Cond (h2s2c) chains and is written to ``<cache-root>/<target>/<arch>/<name>.chains``.
p4symbex then loads it via ``--state-dep-cache`` (wired through ``tampering.py
--state-dep-cache-root``) instead of re-analyzing.

Two builders (``--builder``):
  * ``p4symbex`` (default): ``p4symbex --dump-state-dep-cache`` runs the SAME in-process analysis on
    the SAME post-midend IR that a later ``--state-dep-cache`` load re-resolves against, so cached
    node positions align by construction (p4symbex self-checks this before exit). This eliminates the
    "position not found in program IR" mismatch that the pre-midend analyzer path can produce.
  * ``analyzer``: legacy ``p4c_state_dependency --supergraph FULL --cache-chains``, which serializes
    positions from its own (pre-midend) IR. Kept for comparison/fallback.

The cache is GLOBAL — one set, reused across nightly current/previous runs — so it lives directly
under ``$OUTROOT/<cache-name>`` (default ``sochain_cache``), NOT under current/previous.

The compile environment (arch, -I/-D flags) mirrors how p4symbex compiles each program. A source-hash
embedded in each cache lets p4symbex hard-error on a stale cache.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import logging
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent  # p4c-e2e-scripts/
sys.path.insert(0, str(ROOT))

from tampering import parse_program_list  # noqa: E402
from tofino.tofino_driver import HOME, P4C_INCLUDE_FLAGS, _target_define  # noqa: E402

log = logging.getLogger("cache_chains")

DEFAULT_BIN = HOME / "Workspace/p4c/build/p4c_state_dependency"
DEFAULT_P4SYMBEX_BIN = HOME / "Workspace/p4c/build/p4symbex"
# (target, arch) cells of the nightly matrix. Both policies of a cell share one cache file, so we
# only need the union of programs; we read both the h2s2k and h2s2c lists to collect them.
DEFAULT_MATRIX = [("bmv2", "v1model"), ("tofino", "v1model"), ("tofino", "tna")]
DEFAULT_LISTS = ["h2s2k", "h2s2c"]


def _compile_flags(spec) -> list:
    """Replicate p4symbex's compile environment so cached node positions match p4symbex's IR.
    Tofino adds the bf-p4c/p4include + p4include search paths and the target define; bmv2's v1model
    is on p4c's default include path. Per-program -I/-D come from the program-list row (extra_args)."""
    if spec.target == "tofino":
        return [_target_define(spec.arch), *P4C_INCLUDE_FLAGS, *spec.extra_args]
    return [*spec.extra_args]


def build_one(spec, cache_root: Path, binp: str, timeout: int, force: bool,
              whole_pipeline: bool = False, parser_deps: bool = False,
              builder: str = "p4symbex"):
    out = cache_root / spec.target / spec.arch / f"{spec.name}.chains"
    try:
        # Cache is fresh only if it is at least as new as BOTH the .p4 source AND the builder
        # binary. Without the binary check, rebuilding the builder (same sources) would leave every
        # entry "fresh" and the cache would silently reflect the OLD binary. (binp is whichever
        # builder is selected, so a p4symbex rebuild invalidates a p4symbex-built cache.)
        src_mtime = Path(spec.p4_file).stat().st_mtime
        bin_mtime = Path(binp).stat().st_mtime
        fresh = (not force and out.exists()
                 and out.stat().st_mtime >= src_mtime
                 and out.stat().st_mtime >= bin_mtime)
    except OSError:
        fresh = False
    if fresh:
        return (spec.name, "skip", 0)
    out.parent.mkdir(parents=True, exist_ok=True)
    std = (f"p4-{spec.p4_version}" if not str(spec.p4_version).startswith("p4-")
           else str(spec.p4_version))
    if builder == "p4symbex":
        # Build the cache from p4symbex's OWN post-midend IR. --dump-state-dep-cache runs the
        # in-process analysis, serializes the chains, and self-checks re-resolution before exiting,
        # so the cache aligns by construction with a later --state-dep-cache load (same lowered IR).
        cmd = [str(binp), "--target", spec.target, "--arch", spec.arch, "--std", std,
               *_compile_flags(spec)]
        if whole_pipeline:
            cmd.append("--whole-pipeline")
        if parser_deps:
            cmd.append("--parser-deps")
        cmd += ["--dump-state-dep-cache", str(out), spec.p4_file]
    else:
        # Legacy analyzer path: p4c_state_dependency serializes chains from its own (pre-midend) IR.
        # Redirect its .dot dumps to a per-program dir under <outroot>/supergraphs/ instead of the
        # process cwd (graphsDir defaults to "." with graphs=true, scattering *_dep.dot into wherever
        # the sweep runs). Per-program subdir keeps them from colliding.
        graphs_dir = cache_root.parent / "supergraphs" / spec.target / spec.arch / spec.name
        graphs_dir.mkdir(parents=True, exist_ok=True)
        cmd = [str(binp), "--arch", spec.arch, "--std", std,
               *_compile_flags(spec), "--graphs-dir", str(graphs_dir), "--supergraph", "FULL"]
        if whole_pipeline:
            # Whole-pipeline analysis (Parser->Ingress/Egress) so cross-block chains are cached.
            cmd.append("--whole-pipeline")
        if parser_deps:
            # Parser-state dependency record: seed parser-derived metadata as sources + header pins.
            cmd.append("--parser-deps")
        cmd += ["--cache-chains", str(out), spec.p4_file]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return (spec.name, "TIMEOUT", 124)
    if r.returncode == 0 and out.exists():
        return (spec.name, "ok", 0)
    # On failure (incl. a p4symbex self-check failure, which writes the cache THEN errors), delete
    # any partial cache so a later run doesn't mistake its fresh mtime for a good build.
    try:
        out.unlink()
    except OSError:
        pass
    return (spec.name, f"FAIL(rc={r.returncode})", r.returncode)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outroot", required=True,
                    help="$OUTROOT; the cache is written under <outroot>/<cache-name>")
    ap.add_argument("--cache-name", default="sochain_cache")
    ap.add_argument("--program-lists",
                    help="directory containing <list>_program_list.txt files (matrix mode)")
    ap.add_argument("--program-list",
                    help="a single arbitrary list file (e.g. program_list.txt) to build over; rows are "
                         "filtered to the supported (target,arch) cells. Alternative to --program-lists.")
    ap.add_argument("--lists", nargs="*", default=DEFAULT_LISTS)
    ap.add_argument("--whole-pipeline", action="store_true",
                    help="analyze the whole pipeline (Parser->Ingress/Egress) so cross-block chains "
                         "are cached. Use a distinct --cache-name (e.g. sochain_cache_wp).")
    ap.add_argument("--parser-deps", action="store_true",
                    help="parser-state dependency mode: seed parser-derived metadata as sources + cache "
                         "per-chain header pins. Use a distinct --cache-name (e.g. sochain_cache_pd).")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=3600, help="per-program seconds")
    ap.add_argument("--filter", default=None, help="substring match on program name (include)")
    ap.add_argument("--exclude", default=None,
                    help="skip programs whose name contains this substring (e.g. tango)")
    ap.add_argument("--archs", nargs="*", default=None,
                    help="restrict the matrix to these archs (e.g. --archs v1model); default all")
    ap.add_argument("--targets", nargs="*", default=None,
                    help="restrict the matrix to these targets (e.g. --targets bmv2 tofino); default all")
    ap.add_argument("--builder", choices=("p4symbex", "analyzer"), default="p4symbex",
                    help="how to build the cache. 'p4symbex' (default) uses "
                         "p4symbex --dump-state-dep-cache (post-midend IR, self-checked re-resolution); "
                         "'analyzer' uses the legacy p4c_state_dependency --cache-chains (pre-midend).")
    ap.add_argument("--bin", default=str(DEFAULT_BIN),
                    help="p4c_state_dependency binary (used only with --builder analyzer)")
    ap.add_argument("--p4symbex-bin", default=str(DEFAULT_P4SYMBEX_BIN),
                    help="p4symbex binary (used with --builder p4symbex)")
    ap.add_argument("--force", action="store_true", help="rebuild even if the cache looks fresh")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    if not args.program_lists and not args.program_list:
        ap.error("one of --program-lists (matrix dir) or --program-list (single file) is required")

    cache_root = Path(args.outroot).expanduser() / args.cache_name
    # Effective builder binary: p4symbex for the default post-midend path, else the legacy analyzer.
    builder_bin = args.p4symbex_bin if args.builder == "p4symbex" else args.bin

    # Restrict the (target, arch) matrix if requested (e.g. --archs v1model to regen only v1model).
    matrix = list(DEFAULT_MATRIX)
    if args.archs:
        matrix = [(t, a) for (t, a) in matrix if a in args.archs]
    if args.targets:
        matrix = [(t, a) for (t, a) in matrix if t in args.targets]

    # Collect programs, deduped by (target, arch, name). parse_program_list filters each file to the
    # given (target,arch), so iterating the supported cells over a single file picks up all its rows.
    seen = {}
    if args.program_list:
        pl = Path(args.program_list).expanduser()
        for target, arch in matrix:
            for spec in parse_program_list(pl, target, arch):
                if args.filter and args.filter not in spec.name:
                    continue
                if args.exclude and args.exclude in spec.name:
                    continue
                seen.setdefault((spec.target, spec.arch, spec.name), spec)
    else:
        tr = Path(args.program_lists).expanduser()
        for target, arch in matrix:
            for lst in args.lists:
                pl = tr / f"{lst}_program_list.txt"
                if not pl.exists():
                    continue
                for spec in parse_program_list(pl, target, arch):
                    if args.filter and args.filter not in spec.name:
                        continue
                    if args.exclude and args.exclude in spec.name:
                        continue
                    seen.setdefault((spec.target, spec.arch, spec.name), spec)
    specs = list(seen.values())
    log.info("[cache_chains] %d programs -> %s (builder=%s, jobs=%d, timeout=%ds)",
             len(specs), cache_root, args.builder, args.jobs, args.timeout)

    counts = {"ok": 0, "skip": 0}
    fails = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(build_one, s, cache_root, builder_bin, args.timeout, args.force,
                          args.whole_pipeline, args.parser_deps, args.builder)
                for s in specs]
        for f in concurrent.futures.as_completed(futs):
            name, status, _ = f.result()
            log.info("  %-58s %s", name, status)
            if status in counts:
                counts[status] += 1
            else:
                fails.append((name, status))
    log.info("[cache_chains] ok=%d skip=%d fail=%d", counts["ok"], counts["skip"], len(fails))
    for name, status in fails:
        log.warning("  FAILED: %s (%s)", name, status)
    # Non-fatal: the sweep still runs; programs whose cache failed hard-error visibly under
    # --state-dep-cache-root, which is the intended strict behavior.
    return 0


if __name__ == "__main__":
    sys.exit(main())
