#!/usr/bin/env bash
# Phase-1 accumulation regression for the analytical drive-register path
# (core/symbolic_executor/state_dependency_track.cpp::driveRegisterPhase2 + runConditionChain routing).
#
# Fixture: accum_threshold_tna.p4 — a per-packet counter gated by threshold C=20 feeding an
# if-condition sink (H2S2C). A single Phase-2 write cannot cross the threshold; the tamper is only
# realizable by replaying the Phase-2 packet k times. The analytical driver solves k = C+1 = 21 and
# the emitter writes it as `repeat_count: 21` (k>16). A single-packet emission (no repeat_count)
# would be a false positive that cannot cross the threshold on hardware.
#
# Generation-only (no Tofino HW replay on this machine): success = an emitted `repeat_count > 1`
# and the "analytical drive-register" driver message.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
P4="$HERE/accum_threshold_tna.p4"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
out="$tmp/out"; log="$tmp/gen.log"; mkdir -p "$out"

timeout 500 "$SX" --target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}" \
    --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS \
    --max-tests 4 --max-port 32 --out-dir "$out" \
    --state-dep --path-selection STATE_DEP_TAMPERING_COND "$P4" \
    --state-tamper-value 0xdeadbeef > "$log" 2>&1
rc=$?

n=$(find "$out" -name '*.txtpb' | wc -l | tr -d ' ')
rep=$(grep -rho 'repeat_count: [0-9]*' "$out" --include='*.txtpb' | sort -u | tr '\n' ' ')
maxrep=$(grep -rho 'repeat_count: [0-9]*' "$out" --include='*.txtpb' | awk '{print $2}' | sort -n | tail -1)
drive=$(grep -c 'analytical drive-register' "$log")

fail=0
if [ "$rc" -ne 0 ]; then echo "FAIL: p4symbex exit $rc"; tail -5 "$log" | sed 's/^/    /'; fail=1
elif [ "$n" -eq 0 ]; then echo "FAIL: no txtpb emitted (chain did not form / yield)"; tail -5 "$log" | sed 's/^/    /'; fail=1
elif [ "$drive" -lt 1 ]; then echo "FAIL: analytical drive-register did not fire"; fail=1
elif [ -z "$maxrep" ] || [ "$maxrep" -le 1 ]; then
    echo "FAIL: no repeat_count>1 emitted (single-packet false positive); repeat_counts=[$rep]"; fail=1
else
    echo "PASS: accumulation emitted repeat_count=[$rep] (driver fired x$drive), $n txtpb"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
