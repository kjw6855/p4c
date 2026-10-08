#!/usr/bin/env python3
"""Compare p4c_chain_extractor output with p4c_state_dependency output.

Both directories hold one program's outputs from a run with
`--graphs-dir <dir> --cache-chains <dir>/chains.json`, with stdout saved as <dir>/stdout.txt.

  --equal     p4ccx must reproduce p4csd (use with p4ccx --legacy-flow)
  --superset  p4ccx must contain everything p4csd found (default p4ccx flow)

Compared, order-insensitively:
  * chains.json  key (H2S2K) / cond (H2S2C) chains, ignoring chain ids
  * *_dep.dot    dependency-graph edges by node label (chain-id satellites ignored;
                 --superset skips the *_merged_* graphs, whose node merging depends on all edges)
  * stdout       per-control H2S2K/H2S2C/A2S2K/A2S2C counts and SOs[...] sets
Exit status 0 when the relation holds; differences are printed.
"""

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

NODE_RE = re.compile(r'^(\d+)\[(.*)\];$')
EDGE_RE = re.compile(r'^(\d+)\s*->\s*(\d+)\s*(?:\[(.*)\])?;$')
LABEL_RE = re.compile(r'label=("(?:[^"\\]|\\.)*"|[^,\]]+)')
COUNT_RE = re.compile(r'^\s+(H2S2K|H2S2C|A2S2K|A2S2C)\s.*:\s+(\d+)$')
SOS_RE = re.compile(r'^\s+SOs\[([^\]]+)\]:(.*)$')


def chain_key(c):
    return (c['soName'], c['sinkTable'], c['sinkKey'], c['isUpdate'], tuple(sorted(c['write'])),
            tuple(sorted(c['read'])), c['cond'],
            tuple(sorted(json.dumps(d, sort_keys=True) for d in c.get('parserDeps', []))))


def load_chains(d):
    p = d / 'chains.json'
    if not p.exists():
        return {}
    root = json.loads(p.read_text())
    out = {}
    for cat in ('key', 'cond'):
        for ctrl, chains in root.get(cat, {}).items():
            out[(cat, ctrl)] = Counter(chain_key(c) for c in chains)
    return out


def load_dot_edges(path):
    labels, satellites, edges = {}, set(), Counter()
    for line in path.read_text().splitlines():
        line = line.strip()
        m = NODE_RE.match(line)
        if m:
            attrs = m.group(2)
            lm = LABEL_RE.search(attrs)
            labels[m.group(1)] = lm.group(1) if lm else m.group(1)
            if 'shape=circle' in attrs:
                satellites.add(m.group(1))
            continue
        m = EDGE_RE.match(line)
        if m:
            a, b = m.group(1), m.group(2)
            if a in satellites or b in satellites:
                continue
            edges[(a, b, m.group(3) or '')] += 1
    return Counter({(labels.get(a, a), labels.get(b, b), attr): n
                    for (a, b, attr), n in edges.items()})


def load_dots(d):
    return {p.name: load_dot_edges(p) for p in sorted(d.glob('*_dep.dot'))}


def load_stdout(d):
    counts, sos, ctrl = {}, {}, None
    p = d / 'stdout.txt'
    if not p.exists():
        return counts, sos
    for line in p.read_text().splitlines():
        s = line.strip()
        if s.startswith('[') and s.endswith(']') and not s.startswith('[RESULT'):
            ctrl = s[1:-1]
        elif s == '--- Total ---':
            ctrl = 'Total'
        m = COUNT_RE.match(line)
        if m and ctrl:
            counts[(ctrl, m.group(1))] = int(m.group(2))
        m = SOS_RE.match(line)
        if m:
            sos[m.group(1)] = set(m.group(2).split())
    return counts, sos


def contains(big, small):
    return all(big[k] >= n for k, n in small.items())


def compare(csd, ccx, superset):
    errors = []
    rel = 'superset of' if superset else 'equal to'

    a, b = load_chains(csd), load_chains(ccx)
    for k in sorted(set(a) | set(b)):
        ca, cb = a.get(k, Counter()), b.get(k, Counter())
        ok = contains(cb, ca) if superset else ca == cb
        if not ok:
            errors.append(f'chains {k}: p4ccx {sum(cb.values())} not {rel} p4csd {sum(ca.values())}')

    da, db = load_dots(csd), load_dots(ccx)
    for name in sorted(set(da) | set(db)):
        # A merged graph collapses variable-less nodes, which depends on the whole edge set, so
        # extra edges can re-shape it; inclusion is only meaningful for the unmerged graphs.
        if superset and '_merged_' in name:
            continue
        ea, eb = da.get(name, Counter()), db.get(name, Counter())
        ok = contains(eb, ea) if superset else ea == eb
        if not ok:
            missing = sum((ea - eb).values())
            extra = sum((eb - ea).values())
            errors.append(f'{name}: {missing} edges missing, {extra} extra in p4ccx')

    (na, sa), (nb, sb) = load_stdout(csd), load_stdout(ccx)
    for k in sorted(set(na) | set(nb)):
        va, vb = na.get(k, 0), nb.get(k, 0)
        if (vb < va) if superset else (va != vb):
            errors.append(f'count {k}: p4csd {va}, p4ccx {vb}')
    for k in sorted(set(sa) | set(sb)):
        va, vb = sa.get(k, set()), sb.get(k, set())
        exact = k == 'ALL' or k.startswith('TYPE:')
        if (not vb >= va) if (superset and not exact) else (va != vb):
            errors.append(f'SOs[{k}]: missing {sorted(va - vb)}, extra {sorted(vb - va)}')
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--equal', action='store_true')
    mode.add_argument('--superset', action='store_true')
    ap.add_argument('p4csd_dir', type=Path)
    ap.add_argument('p4ccx_dir', type=Path)
    args = ap.parse_args()
    errors = compare(args.p4csd_dir, args.p4ccx_dir, args.superset)
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
