#!/usr/bin/env bash
# Control-plane action-parameter threshold regression for the key-sink drive-register path
# (core/small_step/table_stepper.cpp::cpActionArgPin + state_dependency_track.cpp::driveRegisterPhase2).
#
# Fixture: cp_action_param_tna.p4 -- a bare `v = v + 1` RegisterAction (NO constant relation in the
# write path at all) whose counter is compared against a threshold supplied as a control-plane action
# parameter, feeding a TABLE KEY sink (H2S2K). This is the SketchLib countmin shape.
#
# What each assertion is guarding:
#   pin       -- cpActionArgPin fired. Annotations name actions/parameters as the control plane sees
#                them ("set_threshold", "threshold") while the IR carries qualified action names and
#                midend-uniquified parameter names ("threshold_1"); a naive comparison silently
#                matches nothing and the whole feature becomes a no-op that still looks green.
#   entry     -- the EMITTED test really carries threshold = 20. Without the synthesis-time pin the
#                driver can still derive k from the annotated value while the emitted entry says
#                threshold = 0 -- an internally inconsistent test that cannot replay. This assertion
#                is the one that catches that, and it caught it during development.
#   repeat    -- k was actually derived and emitted. 20 (at threshold) or 21 (strictly above); both
#                are admitted because the driver tries the Geq and Grt readings and lets validation
#                choose.
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
P4="$HERE/cp_action_param_tna.p4"
ANN="$HERE/cp_action_param_tna.json"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
out="$tmp/out"; log="$tmp/gen.log"; mkdir -p "$out"

timeout 900 "$SX" --target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}" \
    --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS \
    --max-tests 4 --max-port 32 --out-dir "$out" \
    --state-dep --path-selection STATE_DEP_TAMPERING \
    --cp-annotation "$ANN" "$P4" \
    --state-tamper-value 0x1 > "$log" 2>&1
rc=$?

n=$(find "$out" -name '*.txtpb' | wc -l | tr -d ' ')
pin=$(grep -c 'pinning action data' "$log")
# The emitted entry's threshold, as the generated test would install it.
badthr=$(grep -rho 'ig_md.threshold = threshold;| Computed: ig_md.threshold = [0-9]*' "$out" \
             --include='*.txtpb' | awk '{print $NF}' | sort -u | grep -vc '^20$')
reps=$(grep -rho 'repeat_count: [0-9]*' "$out" --include='*.txtpb' | awk '{print $2}' | sort -un | tr '\n' ' ')
goodrep=$(grep -rho 'repeat_count: [0-9]*' "$out" --include='*.txtpb' | awk '{print $2}' \
              | grep -cE '^(20|21)$')

fail=0
if [ "$rc" -ne 0 ]; then
    echo "FAIL: p4symbex exit $rc"; tail -5 "$log" | sed 's/^/    /'; fail=1
elif [ "$n" -eq 0 ]; then
    echo "FAIL: no txtpb emitted (chain did not form / yield)"; tail -5 "$log" | sed 's/^/    /'; fail=1
else
    if [ "$pin" -lt 1 ]; then
        echo "FAIL: cpActionArgPin did not fire -- the action_data clause matched nothing"; fail=1
    else
        echo "PASS: pin fired (x$pin)"
    fi
    if [ "$badthr" -ne 0 ]; then
        echo "FAIL: $badthr emitted test(s) carry a threshold other than 20 -- entry and k disagree"
        fail=1
    else
        echo "PASS: every emitted test carries threshold = 20"
    fi
    if [ "$goodrep" -lt 1 ]; then
        echo "FAIL: no emitted repeat_count of 20 or 21; saw [$reps]"; fail=1
    else
        echo "PASS: emitted repeat_count in {20,21} (x$goodrep); all seen: [$reps]"
    fi
    echo "      $n txtpb emitted"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
