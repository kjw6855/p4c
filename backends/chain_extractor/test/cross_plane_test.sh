#!/usr/bin/env bash
# Regression test for A2S chains of control-plane updates in p4c_chain_extractor.
#
# cross_plane_tna.p4 writes table action params into registers and reads them back into table keys:
# inc_reg and rad_reg take the param through a RegisterAction body (`v = ig_md.v`) and read it at a
# header / param index; res_reg is written and read at a param index; rmw_reg is a read-modify-write
# at a param index. cross_plane_v1model.p4 writes a param as data and reads it at a header / constant
# index. Each must give an a2s2k chain to its t_use_* table: a control-plane update leaves the written
# state stale for every reader, whatever its index. H2S keeps requiring a header-reached read index.
#
# Override the binary with P4CX_BIN=/path/to/p4c_chain_extractor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4CX_BIN:-$ROOT/build/p4c_chain_extractor}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

"$BIN" --arch tna --std p4-16 -D__TARGET_TOFINO__=1 --target tofino \
    -I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/tna.json" "$HERE/cross_plane_tna.p4" >/dev/null 2>&1
"$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/v1.json" "$HERE/cross_plane_v1model.p4" >/dev/null 2>&1

python3 -I - "$tmp/tna.json" "$tmp/v1.json" <<'PY'
import json, sys

tna, v1 = (json.load(open(f)) for f in sys.argv[1:3])

def chains(cache, cat, so, sink, ctrl='Ingress'):
    return [c for g in cache.get(cat, {}).values() for c in g
            if c['soName'] == f'{ctrl}.{so}' and c['sinkTable'] == f'{ctrl}.{sink}']

checks = [
    ('a2s2k: set_v -> RegisterAction body write -> inc_reg read at a header index -> t_use_inc',
     bool(chains(tna, 'a2s2k', 'inc_reg', 't_use_inc'))),
    ('a2s2k: set_v -> RegisterAction body write -> rad_reg -> t_use_rad',
     bool(chains(tna, 'a2s2k', 'rad_reg', 't_use_rad'))),
    ('a2s2k: set_slot index -> res_reg -> t_use_res',
     bool(chains(tna, 'a2s2k', 'res_reg', 't_use_res'))),
    ('a2s2k: set_slot index -> rmw_reg read-modify-write -> t_use_rmw',
     any(c['isUpdate'] for c in chains(tna, 'a2s2k', 'rmw_reg', 't_use_rmw'))),
    ('v1model a2s2k: set_v -> hdr_reg read at a header index -> t_use_hdr',
     bool(chains(v1, 'a2s2k', 'hdr_reg', 't_use_hdr', 'I'))),
    ('v1model a2s2k: set_v -> const_reg read at a constant index -> t_use_const',
     bool(chains(v1, 'a2s2k', 'const_reg', 't_use_const', 'I'))),
    ('v1model h2s2k: no chain through a constant read index',
     not chains(v1, 'h2s2k', 'const_reg', 't_use_const', 'I')),
]
failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(('PASS ' if ok else 'FAIL ') + name)
sys.exit(1 if failed else 0)
PY
