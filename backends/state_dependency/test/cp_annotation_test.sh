#!/usr/bin/env bash
# Regression for --cp-annotation (external control-plane / port annotations).
#
# The load-bearing claim is that an annotation changes WORK and LABELS, never results — except
# where a control-plane assumption proves a candidate unrealizable, which must prune it entirely.
#
# Two proven-emitting fixtures are reused rather than authoring a third program: a fresh .p4 that
# silently emits zero tests would make every assertion below vacuously pass.
#   search_opt_tna.p4  — 4 condition chains (regA..regD) -> exercises the port verdict classes
#   lock_flip_tna.p4   — keyed sink table lock_tbl        -> exercises CP assumption pruning
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
COMMON=(--target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}"
        --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS
        --max-tests 4 --max-port 32 --state-dep)
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

# gen <name> <program.p4> <policy> <annotation|-> [extra...]
gen() {
    local name="$1" prog="$2" pol="$3" ann="$4"; shift 4
    local extra=(); [ "$ann" != "-" ] && extra=(--cp-annotation "$ann")
    mkdir -p "$tmp/$name"
    timeout 900 "$SX" "${COMMON[@]}" --path-selection "$pol" "${extra[@]}" \
        --out-dir "$tmp/$name" "$@" "$HERE/$prog" >"$tmp/$name.log" 2>&1
}
count() { find "$tmp/$1" -name '*.txtpb' 2>/dev/null | wc -l | tr -d ' '; }
# NOTE: grep -c prints 0 AND exits 1 when there is no match, so `|| echo 0` would emit two lines
# and break the arithmetic tests below. Swallow the exit status instead.
prunes() { grep -c 'CP assumption prunes test' "$tmp/$1.log" 2>/dev/null | head -1; }

echo "=== generating ==="
gen port_none search_opt_tna.p4 STATE_DEP_TAMPERING_COND -
gen port_ann  search_opt_tna.p4 STATE_DEP_TAMPERING_COND "$HERE/cp_annotation_port.json"
gen cp_none   lock_flip_tna.p4  STATE_DEP_TAMPERING      -                                   --state-tamper-value 0x1
gen cp_false  lock_flip_tna.p4  STATE_DEP_TAMPERING      "$HERE/cp_annotation_cp_false.json" --state-tamper-value 0x1
gen cp_true   lock_flip_tna.p4  STATE_DEP_TAMPERING      "$HERE/cp_annotation_cp_true.json"  --state-tamper-value 0x1

# ---- 0. the fixtures must actually yield tests, else everything below is vacuous ----
if [ "$(count port_none)" -eq 0 ] || [ "$(count cp_none)" -eq 0 ]; then
    echo "FAIL [fixture]: a baseline emitted no txtpb (port_none=$(count port_none) cp_none=$(count cp_none))"
    fail=1
fi

# ---- 1. LABELS DO NOT CHANGE RESULTS ----
if [ "$(count port_none)" -ne "$(count port_ann)" ]; then
    echo "FAIL [label-parity]: $(count port_none) vs $(count port_ann) txtpb — labelling must not change results"
    fail=1
else
    echo "PASS [label-parity]: $(count port_ann) txtpb with and without the annotation"
fi

# ---- 2. every port verdict class is reachable ----
missing=""
for v in unauthorized partitioned authorized; do
    grep -q "Port authorization:.*verdict=$v" "$tmp/port_ann.log" || missing="$missing $v"
done
if [ -n "$missing" ]; then
    echo "FAIL [port-verdicts]: never produced:$missing"
    fail=1
else
    echo "PASS [port-verdicts]: unauthorized + partitioned + authorized all produced"
fi
if grep -q 'Port authorization' "$tmp/port_none.log"; then
    echo "FAIL [port-quiet]: labels emitted with no --cp-annotation"; fail=1
else
    echo "PASS [port-quiet]: no labels without the flag"
fi

# ---- 3. a FALSE control-plane assumption prunes every candidate ----
if [ "$(count cp_false)" -ne 0 ] || [ "$(prunes cp_false)" -eq 0 ]; then
    echo "FAIL [cp-prune]: expected 0 txtpb and >0 prunes, got $(count cp_false) txtpb / $(prunes cp_false) prunes"
    fail=1
else
    echo "PASS [cp-prune]: false assumption pruned all candidates ($(prunes cp_false) prunes, 0 txtpb)"
fi

# ---- 4. a TRUTHFUL assumption prunes nothing (guards against over-pruning) ----
if [ "$(count cp_true)" -ne "$(count cp_none)" ] || [ "$(prunes cp_true)" -ne 0 ]; then
    echo "FAIL [cp-sound]: truthful assumption changed the yield: $(count cp_none) -> $(count cp_true)"
    echo "       an assumption consistent with the program must never prune"
    fail=1
else
    echo "PASS [cp-sound]: truthful assumption pruned nothing ($(count cp_true) txtpb, unchanged)"
fi

echo
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT"
exit "$fail"
