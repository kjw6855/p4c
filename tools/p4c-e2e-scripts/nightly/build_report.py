#!/usr/bin/env python3
"""Scan a nightly sweep run directory and emit an RFC822 email (HTML table) to stdout.

Usage: build_report.py <RUN_DIR>   # RUN_DIR holds <target>_<arch>_<list>/ subdirs
The output is piped to `msmtp <recipient>` by nightly_sweep.sh.

Table columns: program | target | arch | num txtpb | dir (txtpb+logs) | description
"""
import os, sys, re, html
from pathlib import Path
from email.utils import formatdate
from datetime import datetime

# Recipient/From for the report headers. Kept out of the repo: each VM provides it
# via $SWEEP_MAILTO (set in the cron line). Falls back to a neutral placeholder.
RECIP = os.environ.get("SWEEP_MAILTO") or "p4symbex-nightly@localhost"


def classify(prog_dir: Path, n_txtpb: int) -> str:
    """One-line description from the per-program p4symbex log(s) + txtpb count."""
    if n_txtpb > 0:
        return f"generated {n_txtpb}"
    text = ""
    for lg in prog_dir.rglob("p4symbex*.log"):
        try:
            text += lg.read_text(errors="ignore")
        except OSError:
            pass
    if not text:
        # bmv2 logs land elsewhere; fall back to any *.log under the program dir.
        for lg in prog_dir.rglob("*.log"):
            try:
                text += lg.read_text(errors="ignore")
            except OSError:
                pass
    if "Compiler Bug" in text:
        return "symbex crash (Compiler Bug)"
    if re.search(r"[0-9]\): error:|^error:", text, re.M):
        return "p4c compile error"
    if "produced no chains" in text:
        return "no SD chains"
    if "Phase 1 found no terminal state" in text:
        return "phase-1 unrealizable (no terminal)"
    if "not satisfiable in a single packet" in text:
        return "phase-2 unsat (needs accumulation)"
    if "Concolic constraints for this path are unsatisfiable" in text:
        return "phase-3 concolic unsat"
    if text:
        return "no tests emitted"
    return "not run / no log"


def count_txtpb(prog_dir: Path) -> int:
    return sum(1 for _ in prog_dir.rglob("*.txtpb"))


def main() -> int:
    run = Path(sys.argv[1])
    rows = []  # (program, target, arch, n, dir, desc)
    for sub in sorted(p for p in run.iterdir() if p.is_dir()):
        # subdir name: <target>_<arch>_<list>  (target/arch have no underscore; list = h2s2k|h2s2c)
        m = re.match(r"^(bmv2|tofino|dpdk)_(v1model|tna|t2na|pna)_(h2s2[kc])$", sub.name)
        if not m:
            continue
        target, arch, _list = m.groups()
        for prog in sorted(p for p in sub.iterdir() if p.is_dir()):
            n = count_txtpb(prog)
            rows.append((prog.name, target, arch, n, str(prog), classify(prog, n)))

    total_tx = sum(r[3] for r in rows)
    gen = sum(1 for r in rows if r[3] > 0)
    date = datetime.now().strftime("%Y-%m-%d")

    # --- HTML body ---
    def td(x, mono=False):
        s = html.escape(str(x))
        return f'<td style="border:1px solid #ccc;padding:4px 8px;{"font-family:monospace;font-size:12px;" if mono else ""}">{s}</td>'
    head = "".join(
        f'<th style="border:1px solid #ccc;padding:4px 8px;background:#f0f0f0;text-align:left;">{h}</th>'
        for h in ["program", "target", "arch", "num txtpb", "dir (txtpb + logs)", "description"])
    body_rows = []
    for prog, tgt, arch, n, d, desc in rows:
        bg = "background:#eafbea;" if n > 0 else ""
        body_rows.append(
            f'<tr style="{bg}">' + td(prog) + td(tgt) + td(arch) +
            f'<td style="border:1px solid #ccc;padding:4px 8px;text-align:right;font-weight:{"bold" if n else "normal"};">{n}</td>' +
            td(d, mono=True) + td(desc) + "</tr>")
    table = (f'<table style="border-collapse:collapse;font-family:sans-serif;font-size:13px;">'
             f'<tr>{head}</tr>{"".join(body_rows)}</table>')
    summary = (f"<p style='font-family:sans-serif'>Nightly p4symbex tampering sweep — <b>{date}</b><br>"
               f"{gen}/{len(rows)} programs generated tests; {total_tx} txtpb total.<br>"
               f"Run dir: <code>{html.escape(str(run))}</code> (only the previous run is also kept).</p>")
    html_body = f"<html><body>{summary}{table}</body></html>"

    msg = (
        f"To: {RECIP}\n"
        f"From: {RECIP}\n"
        f"Subject: [p4symbex] nightly sweep {date} — {gen}/{len(rows)} generated, {total_tx} txtpb\n"
        f"Date: {formatdate(localtime=True)}\n"
        f"MIME-Version: 1.0\n"
        f"Content-Type: text/html; charset=utf-8\n"
        f"\n{html_body}\n"
    )
    sys.stdout.write(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
