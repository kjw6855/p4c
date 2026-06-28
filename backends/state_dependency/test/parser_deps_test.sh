#!/usr/bin/env bash
# Regression test for --parser-deps (parser-state dependency record -> per-control IFDS sources).
#
# Asserts: single-control analysis finds NO cond chain for parser_deps_v1model.p4 (meta.idx/meta.data
# unsourced), while --parser-deps finds one (seeded from the parser-state record). See the .p4 for details.
#
# Override the binary with P4SD_BIN=/path/to/p4c_state_dependency.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4SD_BIN:-$ROOT/build/p4c_state_dependency}"
P4="$HERE/parser_deps_v1model.p4"

cond() {  # $1 = extra flags; prints the "(4) DATA writes to cond" total
    "$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL $1 "$P4" 2>/dev/null \
        | awk '/--- Total ---/{t=1} t && /DATA writes to cond/{print $NF; exit}'
}

pc=$(cond ""); pc=${pc:-0}
pd=$(cond "--parser-deps"); pd=${pd:-0}
echo "per-control cond=$pc  --parser-deps cond=$pd"

if [ "$pc" -eq 0 ] && [ "$pd" -ge 1 ]; then
    echo "PASS: --parser-deps recovers the parser->control chain single-control misses"
    exit 0
fi
echo "FAIL: expected per-control=0 and --parser-deps>=1"
exit 1
