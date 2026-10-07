#!/usr/bin/env python3
"""Categorize corpus programs into h2s2k / h2s2c from a SOChain cache, and diff against the curated lists.

Reads each ``<cache>/<target>/<arch>/<name>.chains`` JSON (built by cache_chains.py) and counts its Key
chains (-> h2s2k) and Cond chains (-> h2s2c). Compares the result with the current curated lists to report
ADDITIONS (chain-bearing, not yet listed) and MISSING (listed but the cache shows 0 / wasn't built — likely
a timeout/failure, NOT necessarily a real removal). With --wp-cache it also reports programs that gain chains
only under whole-pipeline analysis.

Source rows for any written list come from --program-list (so target/arch/extra_args are preserved).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Per-program p4symbex per-policy timeout (seconds), auto-assigned from a program's total (key+cond)
# chain count. Chain-rich programs (e.g. the Mew-prototype family, 70+ chains) are far slower to
# generate, so they get a larger cap; simple programs keep the historical 30 min. Emitted into each
# program-list row as a `symbex_timeout=<N>` tag that tampering.py reads. Discrete tiers keep the
# nightly startup "expected timeouts" log cleanly grouped. (thresholds are (max_chains, timeout).)
TIMEOUT_TIERS = [(5, 1800), (20, 3600), (50, 7200)]
TIMEOUT_MAX = 10800  # > the last threshold


def timeout_for(total_chains: int) -> int:
    for max_chains, secs in TIMEOUT_TIERS:
        if total_chains <= max_chains:
            return secs
    return TIMEOUT_MAX


def read_cache(cache_dir: str) -> dict:
    """Return {name: (key_count, cond_count)} from <cache>/<target>/<arch>/<name>.chains."""
    out = {}
    for f in Path(cache_dir).expanduser().rglob("*.chains"):
        try:
            d = json.loads(f.read_text())
        except Exception:
            continue
        key = sum(len(v) for v in d.get("key", {}).values())
        cond = sum(len(v) for v in d.get("cond", {}).values())
        out[f.stem] = (key, cond)
    return out


def rows_by_name(path: str) -> dict:
    """Map program name (first column) -> full program-list row."""
    rows = {}
    for line in Path(path).expanduser().read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        rows[line.split()[0]] = line.rstrip("\n")
    return rows


def list_names(path: str) -> set:
    p = Path(path).expanduser()
    if not p.exists():
        return set()
    return {ln.split()[0] for ln in p.read_text().splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cache", required=True, help="baseline SOChain cache dir")
    ap.add_argument("--wp-cache", help="whole-pipeline cache dir (for the WP-only gain diff)")
    ap.add_argument("--program-list", required=True, help="program_list.txt (source rows)")
    ap.add_argument("--current-h2s2k", required=True)
    ap.add_argument("--current-h2s2c", required=True)
    ap.add_argument("--write-dir", help="if set, write refreshed h2s2k/h2s2c candidate lists here")
    ap.add_argument("--exclude", default=None,
                    help="ignore programs whose name contains this substring (e.g. tango)")
    args = ap.parse_args()

    base = read_cache(args.cache)
    if args.exclude:
        base = {n: v for n, v in base.items() if args.exclude not in n}
    rows = rows_by_name(args.program_list)
    cur_k = list_names(args.current_h2s2k)
    cur_c = list_names(args.current_h2s2c)

    k_progs = {n for n, (k, c) in base.items() if k > 0}
    c_progs = {n for n, (k, c) in base.items() if c > 0}

    def report(label: str, progs: set, cur: set):
        add = sorted(progs - cur)
        missing = sorted(cur - progs)
        print(f"== {label} ==  cache_has={len(progs)}  current_list={len(cur)}  "
              f"additions={len(add)}  missing_from_cache={len(missing)}")
        for n in add:
            tag = "" if n in rows else "  [NO ROW in program_list]"
            print(f"  + {n}  key/cond={base.get(n)}{tag}")
        for n in missing:
            print(f"  - {n}  (listed; cache={base.get(n, 'NOT BUILT — timeout/fail?')})")
        return add, missing

    print(f"[categorize] baseline cache programs: {len(base)}\n")
    report("h2s2k", k_progs, cur_k)
    print()
    report("h2s2c", c_progs, cur_c)

    if args.wp_cache:
        wp = read_cache(args.wp_cache)
        if args.exclude:
            wp = {n: v for n, v in wp.items() if args.exclude not in n}
        wpk = {n for n, (k, c) in wp.items() if k > 0}
        wpc = {n for n, (k, c) in wp.items() if c > 0}
        print(f"\n== whole-pipeline-only gains ==  (in WP cache, not in baseline)")
        for n in sorted(wpk - k_progs):
            print(f"  h2s2k WP-only: {n}  base={base.get(n)}  wp={wp.get(n)}")
        for n in sorted(wpc - c_progs):
            print(f"  h2s2c WP-only: {n}  base={base.get(n)}  wp={wp.get(n)}")

    if args.write_dir:
        wd = Path(args.write_dir).expanduser()
        wd.mkdir(parents=True, exist_ok=True)

        def tagged_row(n: str) -> str:
            # Strip any pre-existing symbex_timeout= tag from the source row, then append the tag
            # computed from this program's total (key+cond) chain count.
            toks = [t for t in rows[n].split() if not t.startswith("symbex_timeout=")]
            key, cond = base.get(n, (0, 0))
            return " ".join(toks) + f" symbex_timeout={timeout_for(key + cond)}"

        def write_list(fn: str, progs: set):
            present = sorted(n for n in progs if n in rows)
            (wd / fn).write_text("\n".join(tagged_row(n) for n in present) + "\n")
            print(f"wrote {wd / fn}  ({len(present)} rows)")

        write_list("h2s2k_program_list.txt", k_progs)
        write_list("h2s2c_program_list.txt", c_progs)

    return 0


if __name__ == "__main__":
    sys.exit(main())
