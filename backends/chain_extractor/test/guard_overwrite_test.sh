#!/usr/bin/env bash
# Regression test: condition chains when the write a condition guards is overwritten later
# (see guard_overwrite_v1model.p4). A chain survives every overwrite that can leave the guarded
# write live: a read in between, an overwrite under another condition or in a table action, an
# else branch, a local initialised in the block. Only a write that every path overwrites before
# any read loses its condition, because the P4 frontend deletes the write, the empty `if` and the
# register read.
#
# Override the binary with P4CX_BIN=/path/to/p4c_chain_extractor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4CX_BIN:-$ROOT/build/p4c_chain_extractor}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

(cd "$tmp" && "$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/v1.json" "$HERE/guard_overwrite_v1model.p4" >/dev/null 2>&1)

python3 -I - "$tmp/v1.json" <<'PY'
import json, sys

cache = json.load(open(sys.argv[1]))
def sos(cat):
    return {c['soName'] for g in cache.get(cat, {}).values() for c in g}

cond, outc = sos('h2s2c'), sos('h2s2outc')
checks = [('C kept: ' + so, 'I.' + so in cond)
          for so in ('r_ctrl', 'r_read', 'r_condow', 'r_else', 'r_local', 'r_actow', 'r_outread')]
checks += [
    ('C removed by the frontend: r_dead', 'I.r_dead' not in cond),
    ('C removed by the frontend: r_out', 'I.r_out' not in cond),
    ('OutC kept: r_outread (write read by a later output condition)', 'I.r_outread' in outc),
    ('OutC removed by the frontend: r_out', 'I.r_out' not in outc),
]
for name, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + name)
sys.exit(0 if all(ok for _, ok in checks) else 1)
PY
