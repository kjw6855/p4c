#!/usr/bin/env bash
# Control-plane action-parameter regression for the KEYLESS (default-action) path
# (core/small_step/table_stepper.cpp::setTableDefaultEntries + ::cpActionArgPin).
#
# Fixture: cp_action_param_keyless_tna.p4 -- cp_action_param_tna.p4 with the threshold table's KEY
# removed, so set_threshold is reachable only as the table's default action. That routes the pin
# through setTableDefaultEntries instead of evalTableControlEntries, a path that previously had no
# coverage on any target, and it is the SwitchV2P `switch_config` shape.
#
# What each assertion is guarding:
#   default   -- the `default_action(T) == A` clause reached TNA. It was implemented in the bmv2
#                table stepper only; the filter now lives in the shared setTableDefaultEntries, so
#                tofino/tofino-v1model/PNA inherit it. Without it the stepper also forks NoAction.
#   noaction  -- no emitted test actually took the NoAction fork. This is the assertion that fails
#                if the filter is applied to the wrong list (or after the branches are built).
#   pin       -- cpActionArgPin fired on the argument minted for the DEFAULT action. Annotations
#                name actions/parameters as the control plane sees them ("set_threshold",
#                "threshold") while the IR carries qualified action names and midend-uniquified
#                parameter names ("threshold_1").
#   entry     -- the EMITTED test really carries threshold = 20, i.e. the pin constrained the
#                branch condition rather than merely being recorded next to it.
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
P4="$HERE/cp_action_param_keyless_tna.p4"
ANN="$HERE/cp_action_param_keyless_tna.json"
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
defact=$(grep -c 'installing annotated default action' "$log")
pin=$(grep -c 'pinning action data' "$log")
# Any emitted test that overrode the default with NoAction escaped the annotated-default filter.
noaction=$(grep -rc 'Overriding default action: NoAction' "$out" --include='*.txtpb' \
               | awk -F: '{s+=$2} END {print s+0}')
# The emitted entry's threshold, as the generated test would install it.
badthr=$(grep -rho 'ig_md.threshold = threshold;| Computed: ig_md.threshold = [0-9]*' "$out" \
             --include='*.txtpb' | awk '{print $NF}' | sort -u | grep -vc '^20$')

fail=0
if [ "$rc" -ne 0 ]; then
    echo "FAIL: p4symbex exit $rc"; tail -5 "$log" | sed 's/^/    /'; fail=1
elif [ "$n" -eq 0 ]; then
    echo "FAIL: no txtpb emitted (chain did not form / yield)"; tail -5 "$log" | sed 's/^/    /'; fail=1
else
    if [ "$defact" -lt 1 ]; then
        echo "FAIL: the default_action clause did not reach this target -- the keyless table still"
        echo "      forks every action"; fail=1
    else
        echo "PASS: annotated default action installed (x$defact)"
    fi
    if [ "$noaction" -ne 0 ]; then
        echo "FAIL: $noaction emitted test(s) overrode the default with NoAction"; fail=1
    else
        echo "PASS: no emitted test took the NoAction fork"
    fi
    if [ "$pin" -lt 1 ]; then
        echo "FAIL: cpActionArgPin did not fire on the default action's argument"; fail=1
    else
        echo "PASS: pin fired (x$pin)"
    fi
    if [ "$badthr" -ne 0 ]; then
        echo "FAIL: $badthr emitted test(s) carry a threshold other than 20"; fail=1
    else
        echo "PASS: every emitted test carries threshold = 20"
    fi
    echo "      $n txtpb emitted"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
