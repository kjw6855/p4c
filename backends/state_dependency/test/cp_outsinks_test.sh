#!/usr/bin/env bash
# Output-chain tampering regression: --state-dep-sinks OUTK/OUTC and the --diverge-output oracle.
#
#   outk.port   cp_outsinks_tna (+snapshot), OUTK, PORT: the sink_a flip (fwd(2) -> drop) is a
#               finding; the sink_b flip (port 2 + ttl rewrite -> port 2) changes no port and is
#               rejected by the gate
#   outk.bytes  same with PORT_AND_BYTES: both sinks are findings
#   outc.port   cp_outc_tna, OUTC, PORT: the counter flip turns "port 2" into "drop" -- a finding
#   outc.neg    accum_threshold_tna, OUTC, PORT: the program drops before and after the flip, so the
#               gate rejects it (the same flip IS a finding for plain C runs)
# plus option errors (category/policy pairing, O not yet supported, --staged-ide).
#
# Generation-only (no target replay).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
TNA=(--target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1
     -I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include" --test-backend BFRT)

run_one() {   # $1 tag, $2 p4 stem, $3... flags
    local tag="$1" stem="$2"; shift 2
    mkdir -p "$tmp/$tag"
    (cd "$tmp/$tag" && timeout 900 "$SX" "${TNA[@]}" --packet-size-range 14:9600 \
        --track-coverage STATEMENTS --max-tests 8 --max-port 32 --out-dir "$tmp/$tag" --state-dep \
        "$@" "$HERE/$stem.p4" --state-tamper-value 0x1 > "$tmp/$tag.log" 2>&1)
    echo $?
}
sinks() { grep -ho 'sink_table: "[^"]*"' "$tmp/$1"/*.txtpb 2>/dev/null | sort -u | sed 's/sink_table: //' | tr '\n' ' '; }
ncases() { find "$tmp/$1" -name '*.txtpb' | wc -l | tr -d ' '; }
gate() { grep -c 'diverge-output PORT' "$tmp/$1.log"; }

fail=0
pass() { echo "PASS [$1]: $2"; }
failed() { echo "FAIL [$1]: $2"; tail -4 "$tmp/$1.log" | sed 's/^/    /'; fail=1; }
SNAP=(--cp-snapshot "$HERE/cp_outsinks_tna.txtpb")

rc=$(run_one outk.port cp_outsinks_tna --path-selection STATE_DEP_TAMPERING --state-dep-sinks OUTK \
     --diverge-output PORT "${SNAP[@]}")
s=$(sinks outk.port)
if [ "$rc" -ne 0 ]; then failed outk.port "p4symbex exit $rc"
elif [ "$s" != '"Ingress.sink_a" ' ]; then failed outk.port "sinks [$s], expected only sink_a"
elif [ "$(gate outk.port)" -lt 1 ]; then failed outk.port "the same-port sink_b flip was not rejected by the gate"
else pass outk.port "sink_a only; sink_b rejected (same port)"; fi

rc=$(run_one outk.bytes cp_outsinks_tna --path-selection STATE_DEP_TAMPERING --state-dep-sinks OUTK \
     --diverge-output PORT_AND_BYTES "${SNAP[@]}")
s=$(sinks outk.bytes)
if [ "$rc" -ne 0 ]; then failed outk.bytes "p4symbex exit $rc"
elif [ "$s" != '"Ingress.sink_a" "Ingress.sink_b" ' ]; then failed outk.bytes "sinks [$s], expected sink_a and sink_b"
else pass outk.bytes "sink_a and sink_b"; fi

rc=$(run_one outc.port cp_outc_tna --path-selection STATE_DEP_TAMPERING_COND --state-dep-sinks OUTC)
if [ "$rc" -ne 0 ]; then failed outc.port "p4symbex exit $rc"
elif [ "$(ncases outc.port)" -lt 1 ]; then failed outc.port "no case for the port-2 -> drop flip"
else pass outc.port "$(ncases outc.port) case(s)"; fi

rc=$(run_one outc.neg accum_threshold_tna --path-selection STATE_DEP_TAMPERING_COND --state-dep-sinks OUTC)
if [ "$rc" -ne 0 ]; then failed outc.neg "p4symbex exit $rc"
elif [ "$(ncases outc.neg)" -ne 0 ] || [ "$(gate outc.neg)" -lt 1 ]; then
    failed outc.neg "$(ncases outc.neg) case(s), gate x$(gate outc.neg): a drop->drop flip must be rejected"
else pass outc.neg "drop -> drop rejected by the gate"; fi

expect_error() {   # $1 tag, $2 message, $3... flags
    local tag="$1" msg="$2"; shift 2
    local rc; rc=$(run_one "$tag" cp_outc_tna "$@")
    if [ "$rc" -eq 0 ] || ! grep -q -- "$msg" "$tmp/$tag.log"; then failed "$tag" "expected an error: $msg"
    else pass "$tag" "rejected"; fi
}
expect_error err.outk.cond "need --path-selection STATE_DEP_TAMPERING" \
    --path-selection STATE_DEP_TAMPERING_COND --state-dep-sinks OUTK
expect_error err.outc.key "need STATE_DEP_TAMPERING_COND" \
    --path-selection STATE_DEP_TAMPERING --state-dep-sinks OUTC
expect_error err.o "not supported yet" --path-selection STATE_DEP_TAMPERING --state-dep-sinks O
expect_error err.staged "staged-ide has none" \
    --path-selection STATE_DEP_TAMPERING --state-dep-sinks OUTK --staged-ide

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
