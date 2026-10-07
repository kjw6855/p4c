#!/usr/bin/env python3
"""RQ funnel aggregator.

Joins the per-program chain/register metrics (run_analysis's metrics.csv) with candidate txtpb
counts (from a p4symbex gen output-root such as p4symbex_nightly/current, H2S only) and, optionally,
replay-confirmed counts (tampering_results.csv OK rows), then reports the corpus funnel + set
overlaps. The join unit is one (name, target, arch) row — the same identity as metrics.csv and the
gen cell dirs — so names match exactly with no normalization guesswork.

    corpus -> has_A2S / has_H2S -> has-chain{A2S2K,A2S2C,H2S2K,H2S2C,any}
           -> candidate (H2S txtpb>0) -> confirmed (replay OK)

Reads only; safe against a live nightly sweep. All writes go to --out-dir.

Usage:
  python3 ./funnel.py --metrics-csv <metrics.csv> --program-list <program_list.txt> \
      --gen-root ~/Workspace-remote/p4symbex_nightly/current \
      [--tampering-csv <tampering_results.csv>] [--out-dir <dir>]
"""
import argparse
import csv
import os
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
if str(_ROOT / "nightly") not in sys.path:
    sys.path.insert(0, str(_ROOT / "nightly"))
from tampering import parse_program_list          # noqa: E402
from build_report import count_txtpb              # noqa: E402

MATRIX = [("bmv2", "v1model"), ("tofino", "tna"), ("tofino", "v1model"), ("dpdk", "pna")]
CHAIN_CATS = ["A2S2K", "A2S2C", "H2S2K", "H2S2C"]


def _num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def load_metrics(path):
    """metrics.csv -> {(name, target, arch): row}."""
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            out[(r["name"], r.get("target", ""), r.get("arch", ""))] = r
    return out


def candidate_count(gen_root, name, target, arch):
    """Sum txtpb across the two H2S gen cells for this (name, target, arch)."""
    total = 0
    for sink in ("h2s2k", "h2s2c"):
        d = gen_root / f"{target}_{arch}_{sink}" / name
        if d.is_dir():
            total += count_txtpb(d)
    return total


def load_confirmed(path):
    """tampering_results.csv -> {target_name: ok_count}. OK = replay-confirmed divergence."""
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("result") == "OK":
                out[r["target_name"]] = out.get(r["target_name"], 0) + 1
    return out


def build_universe(program_list):
    """All corpus rows as (name, target, arch), via the canonical parser over every combo."""
    rows = []
    for tgt, arch in MATRIX:
        for s in parse_program_list(Path(program_list), tgt, arch):
            rows.append((s.name, s.target, s.arch))
    return rows


def main():
    ap = argparse.ArgumentParser(description="RQ funnel aggregator")
    ap.add_argument("--metrics-csv", required=True)
    ap.add_argument("--program-list", required=True)
    ap.add_argument("--gen-root", default=os.path.expanduser(
        "~/Workspace-remote/p4symbex_nightly/current"),
        help="p4symbex gen output-root for candidate txtpb (H2S). Read-only.")
    ap.add_argument("--tampering-csv", default=None,
                    help="optional tampering_results.csv for the confirmed-vuln column")
    ap.add_argument("--out-dir", default=os.path.expanduser(
        "~/Workspace-remote/usenix27/funnel"))
    args = ap.parse_args()

    metrics = load_metrics(args.metrics_csv)
    universe = build_universe(args.program_list)
    gen_root = Path(os.path.expanduser(args.gen_root))
    confirmed = load_confirmed(args.tampering_csv) if args.tampering_csv else None
    out_dir = Path(os.path.expanduser(args.out_dir))
    out_dir.mkdir(parents=True, exist_ok=True)

    per_prog = []
    for (name, target, arch) in universe:
        m = metrics.get((name, target, arch), {})
        cats = {c: int(_num(m.get(c))) for c in CHAIN_CATS}
        cand = candidate_count(gen_root, name, target, arch)
        row = {
            "name": name, "target": target, "arch": arch,
            "has_metrics": int(bool(m)),
            "has_A2S": int(_num(m.get("reg_A2S")) > 0),
            "has_H2S": int(_num(m.get("reg_H2S")) > 0),
            **{c: cats[c] for c in CHAIN_CATS},
            **{f"has_{c}": int(cats[c] > 0) for c in CHAIN_CATS},
            "has_chain_any": int(any(cats[c] > 0 for c in CHAIN_CATS)),
            "candidate_txtpb": cand,
            "is_candidate": int(cand > 0),
            "candidate_registers": int(_num(m.get("candidate_registers"))),
            "reg_A2S_or_H2S": int(_num(m.get("reg_A2S_or_H2S"))),
        }
        if confirmed is not None:
            row["confirmed_ok"] = confirmed.get(name, 0)
            row["is_confirmed"] = int(confirmed.get(name, 0) > 0)
        per_prog.append(row)

    # ---- funnel + overlaps (over rows) ----
    n = len(per_prog)
    cnt = lambda k: sum(r[k] for r in per_prog)
    summary = [
        ("corpus_rows", n),
        ("distinct_programs", len({r["name"] for r in per_prog})),
        ("has_metrics", cnt("has_metrics")),
        ("has_A2S", cnt("has_A2S")),
        ("has_H2S", cnt("has_H2S")),
        ("has_A2S_and_H2S", sum(r["has_A2S"] and r["has_H2S"] for r in per_prog)),
        ("has_A2S_only", sum(r["has_A2S"] and not r["has_H2S"] for r in per_prog)),
        ("has_H2S_only", sum(r["has_H2S"] and not r["has_A2S"] for r in per_prog)),
        ("has_A2S2K", cnt("has_A2S2K")),
        ("has_A2S2C", cnt("has_A2S2C")),
        ("has_H2S2K", cnt("has_H2S2K")),
        ("has_H2S2C", cnt("has_H2S2C")),
        ("has_chain_any", cnt("has_chain_any")),
        ("candidate", cnt("is_candidate")),
        ("total_candidate_txtpb", cnt("candidate_txtpb")),
    ]
    if confirmed is not None:
        summary.append(("confirmed", cnt("is_confirmed")))

    # ---- unmatched-keys diagnostic (mismatch = normalization bug, not data) ----
    uni_names = {r["name"] for r in per_prog}
    metric_names = {k[0] for k in metrics}
    conf_names = set(confirmed) if confirmed is not None else set()
    diag = {
        "metrics_not_in_universe": sorted(metric_names - uni_names),
        "confirmed_not_in_universe": sorted(conf_names - uni_names),
    }

    # ---- write outputs ----
    fields = list(per_prog[0].keys()) if per_prog else []
    with open(out_dir / "funnel_per_program.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(per_prog)
    with open(out_dir / "funnel_summary.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["stage", "count"])
        w.writerows(summary)

    d = dict(summary)
    conf_str = f"confirmed={d['confirmed']}" if confirmed is not None else "confirmed=n/a"
    line = (f"corpus_rows={d['corpus_rows']} (distinct={d['distinct_programs']}) | "
            f"has_A2S={d['has_A2S']} has_H2S={d['has_H2S']} A2S&H2S={d['has_A2S_and_H2S']} | "
            f"has_chain: A2S2K={d['has_A2S2K']} A2S2C={d['has_A2S2C']} H2S2K={d['has_H2S2K']} "
            f"H2S2C={d['has_H2S2C']} any={d['has_chain_any']} | "
            f"candidate(H2S,txtpb>0)={d['candidate']} txtpb={d['total_candidate_txtpb']} | {conf_str}")
    summary_txt = out_dir / "summary.txt"
    with open(summary_txt, "w") as f:
        f.write(line + "\n")
        for k, v in diag.items():
            if v:
                f.write(f"[diag] {k}: {len(v)} -> {v[:20]}\n")
    print(line)
    for k, v in diag.items():
        if v:
            print(f"[diag] {k}: {len(v)} (e.g. {v[:5]})")
    print(f"wrote {out_dir}/funnel_per_program.csv, funnel_summary.csv, summary.txt")


if __name__ == "__main__":
    main()
