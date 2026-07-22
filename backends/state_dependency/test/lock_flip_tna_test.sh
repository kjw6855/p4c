#!/usr/bin/env bash
# Phase-2 tamper-value regression for the forbidden-value override in
# targets/tofino/test_spec.cpp::TofinoRegisterValue::withAttackerValues.
#
# Fixture: lock_flip_tna.p4 — a bit<1> header-indexed lock whose read feeds a table key (H2S2K),
# written from a bit<1> header field (packet-injectable). Phase-1 HIT key is {0} (cells init 0).
#
#   --state-tamper-value 0x0 : the TRUNCATED value 0 collides with the forbidden Phase-1 HIT key, so
#       the fix must override to the flipping value 1 (attacker_value == 1) AND warn. The pre-fix
#       code emitted the forbidden, non-flipping 0.
#   --state-tamper-value 0x1 : 1 is not forbidden, kept as-is (attacker_value == 1), no warning.
#
# Generation-only (no Tofino HW replay on this machine): success = the emitted attacker_value and
# the presence/absence of the override warning.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
COMMON=(--target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}"
        --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS
        --max-tests 4 --max-port 32 --state-dep --path-selection STATE_DEP_TAMPERING)
P4="$HERE/lock_flip_tna.p4"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

# gen <tamper-value> <outdir> <logfile>
gen() {
    local out="$2"; mkdir -p "$out"
    timeout 400 "$SX" "${COMMON[@]}" --out-dir "$out" "$P4" --state-tamper-value "$1" > "$3" 2>&1
}

# WARN CASE: 0x0 truncates into the forbidden HIT key -> must override to 1 + warn.
O0="$tmp/v0"; L0="$tmp/v0.log"; gen 0x0 "$O0" "$L0"
n0=$(find "$O0" -name '*.txtpb' | wc -l | tr -d ' ')
vals0=$(grep -rho 'attacker_value: "\\x0[01]"' "$O0" --include='*.txtpb' | sort -u | tr '\n' ' ')
warn0=$(grep -c 'truncates to a Phase-1 table-key value' "$L0")
if [ "$n0" -eq 0 ]; then
    echo "FAIL [0x0]: no txtpb emitted (chain did not form / yield)"; tail -3 "$L0" | sed 's/^/    /'; fail=1
elif grep -rq 'attacker_value: "\\x00"' "$O0" --include='*.txtpb'; then
    echo "FAIL [0x0]: emitted the FORBIDDEN non-flipping value 0 (pre-fix bug)"; fail=1
elif [ "$warn0" -lt 1 ]; then
    echo "FAIL [0x0]: override warning did not fire (fix branch not taken); values=[$vals0]"; fail=1
else
    echo "PASS [0x0]: override fired (warn x$warn0), attacker_value=[$vals0] (flipping 1, not forbidden 0)"
fi

# KEEP CASE: 0x1 is not forbidden -> kept, no warning.
O1="$tmp/v1"; L1="$tmp/v1.log"; gen 0x1 "$O1" "$L1"
n1=$(find "$O1" -name '*.txtpb' | wc -l | tr -d ' ')
warn1=$(grep -c 'truncates to a Phase-1 table-key value' "$L1")
if [ "$n1" -eq 0 ]; then
    echo "FAIL [0x1]: no txtpb emitted"; tail -3 "$L1" | sed 's/^/    /'; fail=1
elif [ "$warn1" -ne 0 ]; then
    echo "FAIL [0x1]: override warning fired for a non-forbidden value (should keep 0x1 as-is)"; fail=1
elif ! grep -rq 'attacker_value: "\\x01"' "$O1" --include='*.txtpb'; then
    echo "FAIL [0x1]: expected attacker_value 1"; fail=1
else
    echo "PASS [0x1]: kept requested value 1 (no override, no warning)"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
