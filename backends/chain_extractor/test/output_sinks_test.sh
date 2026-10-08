#!/usr/bin/env bash
# Regression test for the output relations (O, OutC, OutK) of p4c_chain_extractor.
#
# output_sinks_v1model.p4 / output_sinks_tna.p4 each read registers at header-chosen indices and
# route the reads to (a) an output variable, (b) a condition guarding an output write (directly, or
# in v1model through an intermediate table) and (c) a decoy condition that writes no output. The
# chain cache must list (a) under h2s2o, (b) under h2s2outc, and (c) under cond but not h2s2outc.
# The v1model fixture also indexes a register by a table action param, which must give a2s2o; the
# tna fixture keys output-writing tables (directly and through an intermediate table), which must
# give h2s2outk, and a decoy table whose actions write no output.
#
# Override the binary with P4CX_BIN=/path/to/p4c_chain_extractor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4CX_BIN:-$ROOT/build/p4c_chain_extractor}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

"$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/v1.json" "$HERE/output_sinks_v1model.p4" >/dev/null 2>&1
"$BIN" --arch tna --std p4-16 -D__TARGET_TOFINO__=1 --target tofino \
    -I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include" --supergraph FULL \
    --cache-chains "$tmp/tna.json" "$HERE/output_sinks_tna.p4" >/dev/null 2>&1

python3 -I - "$tmp/v1.json" "$tmp/tna.json" <<'PY'
import json, sys

def sos(cache, cat):
    return {(c['soName'], c.get('sinkVar', '')) for g in cache.get(cat, {}).values() for c in g}

v1, tna = (json.load(open(p)) for p in sys.argv[1:3])
checks = [
    ('v1 O: r_port -> egress_spec', ('I.r_port', 'sm.egress_spec') in sos(v1, 'h2s2o')),
    ('v1 OutC: r_drop guards mark_to_drop', ('I.r_drop', '') in sos(v1, 'h2s2outc')),
    ('v1 OutC: r_nh via table t_nh', ('I.r_nh', '') in sos(v1, 'h2s2outc')),
    ('v1 decoy is a condition sink', ('I.r_decoy', '') in sos(v1, 'cond')),
    ('v1 decoy is not OutC', ('I.r_decoy', '') not in sos(v1, 'h2s2outc')),
    ('v1 A2S2O: r_cfg (indexed by t_cfg param) -> mcast_grp',
     ('I.r_cfg', 'sm.mcast_grp') in sos(v1, 'a2s2o')),
    ('tna O: port_reg -> ucast_egress_port',
     ('Ingress.port_reg', 'ig_tm_md.ucast_egress_port') in sos(tna, 'h2s2o')),
    ('tna OutC: drop_reg guards drop_ctl', ('Ingress.drop_reg', '') in sos(tna, 'h2s2outc')),
    ('tna decoy is not OutC', ('Ingress.decoy_reg', '') not in sos(tna, 'h2s2outc')),
    ('tna OutK: k2_reg keys t_direct (sets port)', ('Ingress.k2_reg', '') in sos(tna, 'h2s2outk')),
    ('tna OutK: k1_reg via t_mid keys t_out (drops)', ('Ingress.k1_reg', '') in sos(tna, 'h2s2outk')),
    ('tna decoy table key is a key sink', ('Ingress.kdec_reg', '') in sos(tna, 'key')),
    ('tna decoy table key is not OutK', ('Ingress.kdec_reg', '') not in sos(tna, 'h2s2outk')),
]
failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + name)
sys.exit(1 if failed else 0)
PY
