#!/usr/bin/env bash
# Regression test for add-on-miss tables (PNA) as stateful objects in p4c_chain_extractor.
#
# add_on_miss_pna.p4 writes a table action param into the add-on-miss table `flows` with
# add_entry() (the write) and reads it back as the hit action's param (the read), which then keys
# table t_use. The chain cache must list this as an a2s2k chain on MainControlImpl.flows with sink
# table t_use, carrying both its write and its read positions, and tagged (cp) as a cross-plane
# inconsistency of the add-on-miss table, with writeData = t_ptr's set_ptr.ptr.
#
# Override the binary with P4CX_BIN=/path/to/p4c_chain_extractor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4CX_BIN:-$ROOT/build/p4c_chain_extractor}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

"$BIN" --arch pna --target dpdk -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/pna.json" "$HERE/add_on_miss_pna.p4" >/dev/null 2>&1

python3 -I - "$tmp/pna.json" <<'PY'
import json, sys

cache = json.load(open(sys.argv[1]))
chains = [c for g in cache.get('a2s2k', {}).values() for c in g
          if c['soName'] == 'MainControlImpl.flows']
checks = [
    ('a2s2k: t_ptr param -> flows (add_entry) -> flows_hit param -> t_use key',
     any(c['sinkTable'] == 'MainControlImpl.t_use' for c in chains)),
    ('the chain carries the add_entry write', all(c['write'] for c in chains) and bool(chains)),
    ('the chain carries the hit-action read', all(c['read'] for c in chains) and bool(chains)),
    ('add_entry is not a read-modify-write', not any(c['isUpdate'] for c in chains)),
    ('cp: addOnMiss inconsistency, writeData set_ptr.ptr from t_ptr',
     bool(chains) and all(
         c.get('cp', {}).get('soKind') == 'addOnMiss' and c['cp']['inconsistency'] and
         [(s['tables'], s['action'], s['param']) for s in c['cp']['writeData']] ==
         [(['MainControlImpl.t_ptr'], 'MainControlImpl.set_ptr', 'ptr')]
         for c in chains)),
]
failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(('PASS ' if ok else 'FAIL ') + name)
sys.exit(1 if failed else 0)
PY
