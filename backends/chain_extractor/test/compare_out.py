#!/usr/bin/env python3
"""Compare the control-divergence chains [A,H]2S2[K,C] with the output chains [A,H]2S2[OutK,OutC].

Reads p4c_chain_extractor chain caches (--cache-chains) and, per program and family (H = header,
A = action param):
  * checks OutK <= K and OutC <= C (an output sink is a key/condition sink that sets the output);
  * counts how much the output sinks narrow the candidates, in chains and in registers;
  * labels each K/C-only chain: `hop` when its table is an intermediate hop of an output chain from
    the same register access, `no-output` otherwise.

Usage: compare_out.py [--json OUT] [--list] CACHE_OR_CORPUS_DIR...
A directory argument is searched for */chains.json (one subdirectory per program).
Exits 1 when a subset check fails.
"""
import argparse
import glob
import json
import os
import sys

FAMILIES = {
    'H': {'K': 'h2s2k', 'C': 'h2s2c', 'OutK': 'h2s2outk', 'OutC': 'h2s2outc', 'O': 'h2s2o'},
    'A': {'K': 'a2s2k', 'C': 'a2s2c', 'OutK': 'a2s2outk', 'OutC': 'a2s2outc', 'O': 'a2s2o'},
}
# Schema v2 caches (and p4csd's) name H2S2K/H2S2C "key"/"cond".
LEGACY = {'h2s2k': 'key', 'h2s2c': 'cond'}


def chain_id(control, c):
    return (control, c['soName'], c['isUpdate'], tuple(c['write']), tuple(c['read']),
            c['sinkTable'], c['sinkKey'], c['cond'])


def read_id(cid):
    """The register access a chain starts from. Its path ends at the chain's own sink (the last read
    position, or the last write position for an update chain with no reads), so drop that position."""
    control, so, upd, write, read = cid[:5]
    return (control, so, upd, write, read[:-1]) if read else (control, so, upd, write[:-1], ())


def chains(cache, cat):
    """{chain id: chain} for one category."""
    cs_by_ctrl = cache.get(cat, cache.get(LEGACY.get(cat), {}))
    return {chain_id(ctrl, c): c for ctrl, cs in cs_by_ctrl.items() for c in cs}


def compare_family(cache, fam):
    cats = FAMILIES[fam]
    got = {k: chains(cache, cat) for k, cat in cats.items()}
    violations = [(k, cid) for out, base in (('OutK', 'K'), ('OutC', 'C'))
                  for k, cid in ((out, cid) for cid in got[out]) if cid not in got[base]]

    # Tables that some output chain passes through, by the register read it starts from.
    hop_tables = {}
    for k in ('OutK', 'OutC'):
        for cid, c in got[k].items():
            hop_tables.setdefault(read_id(cid), set()).update(h['table'] for h in c.get('hops', []))

    dropped = {'hop': [], 'no-output': []}
    for base, out in (('K', 'OutK'), ('C', 'OutC')):
        for cid in got[base]:
            if cid in got[out]:
                continue
            table = cid[5]
            reason = 'hop' if table and table in hop_tables.get(read_id(cid), ()) else 'no-output'
            dropped[reason].append((base, cid))

    so = lambda *ks: {cid[1] for k in ks for cid in got[k]}
    return {
        'counts': {k: len(v) for k, v in got.items()},
        'so_kc': sorted(so('K', 'C')),
        'so_out': sorted(so('OutK', 'OutC', 'O')),
        'so_dropped': sorted(so('K', 'C') - so('OutK', 'OutC', 'O')),
        'so_only_o': sorted(so('O') - so('K', 'C')),
        'dropped': {r: [{'cat': b, 'control': cid[0], 'soName': cid[1], 'sinkTable': cid[5],
                         'cond': cid[7]} for b, cid in v] for r, v in dropped.items()},
        'violations': [{'cat': k, 'id': list(cid)} for k, cid in violations],
    }


def caches(paths):
    for p in paths:
        if os.path.isdir(p):
            for f in sorted(glob.glob(os.path.join(p, '*', 'chains.json'))):
                yield os.path.basename(os.path.dirname(f)), f
        else:
            yield os.path.basename(p), p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--json', help='write the full per-program result here')
    ap.add_argument('--list', action='store_true', help='print every dropped chain')
    args = ap.parse_args()

    results, failed = {}, False
    cols = ('K', 'OutK', 'C', 'OutC', 'O', 'hop', 'noout', 'SO', 'SOout', 'SOdrop')
    print(f"{'program':44} fam " + ' '.join(f'{c:>6}' for c in cols))
    total = {f: dict.fromkeys(cols, 0) for f in FAMILIES}
    for name, path in caches(args.paths):
        cache = json.load(open(path))
        results[name] = {}
        for fam in FAMILIES:
            r = compare_family(cache, fam)
            results[name][fam] = r
            row = dict(r['counts'], hop=len(r['dropped']['hop']),
                       noout=len(r['dropped']['no-output']), SO=len(r['so_kc']),
                       SOout=len(r['so_out']), SOdrop=len(r['so_dropped']))
            for c in cols:
                total[fam][c] += row[c]
            if any(row.values()):
                print(f'{name[:44]:44} {fam:>3} ' + ' '.join(f'{row[c]:>6}' for c in cols))
            for v in r['violations']:
                failed = True
                print(f'  VIOLATION {fam}{v["cat"]} not in base category: {v["id"]}')
            if args.list:
                for reason, ds in r['dropped'].items():
                    for d in ds:
                        print(f'    {reason:9} {d["cat"]} {d["control"]} {d["soName"]} '
                              f'{d["sinkTable"] or d["cond"]}')
    for fam in FAMILIES:
        print(f"{'TOTAL':44} {fam:>3} " + ' '.join(f'{total[fam][c]:>6}' for c in cols))
    if args.json:
        json.dump(results, open(args.json, 'w'), indent=1)
    print('subset check: ' + ('FAIL' if failed else 'PASS (OutK <= K, OutC <= C)'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
