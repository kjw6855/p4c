#!/usr/bin/env bash
# Regression for reading a TNA register's DECLARED initial value.
#
# targets/tofino/test_spec.cpp::declaredRegisterInitialValue reads the optional second ctor
# argument of Register<T,I>(size, init); shared_expr_stepper seeds a first touch from it instead
# of assuming zero.
#
# reg_init_tna.p4 declares `Register<bit<8>, bit<8>>(256, 7) lock_reg;`, so an untouched cell
# reads as 7 and Phase-1 must HIT lock_tbl on key 7. Seeding 0 -- the old "hardware default"
# assumption -- puts the Phase-1 HIT on key 0, which is the SwitchV2P cache-hit-on-an-empty-slot
# false positive in miniature. A zero-init fixture cannot tell the two apart.
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

"$SX" --target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 \
    -I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include" \
    --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS \
    --max-tests 4 --max-port 32 --state-dep --path-selection STATE_DEP_TAMPERING \
    --state-tamper-value 0x1 --out-dir "$tmp" "$HERE/reg_init_tna.p4" >"$tmp/log" 2>&1
n=$(find "$tmp" -name '*.txtpb' | wc -l | tr -d ' ')

if [ "$n" -eq 0 ]; then
    echo "FAIL [fixture]: emitted no txtpb; every assertion below would be vacuous"
    exit 1
fi
echo "PASS [fixture]: $n txtpb emitted"

# The load-bearing check: Phase 1 reads the DECLARED 7, so the sink HIT key is 7.
if grep -qa 'Table Branch: Ingress.lock_tbl | Key(s): 7' "$tmp"/*.txtpb; then
    echo "PASS [declared-init]: Phase-1 HIT key is 7 (the declared initial value)"
else
    echo "FAIL [declared-init]: Phase-1 did not read the declared 7"
    grep -oa 'Table Branch: Ingress.lock_tbl | Key(s): [0-9]*' "$tmp"/*.txtpb | sort -u | head -3
    fail=1
fi

# Seeding 0 is precisely the regression this fixture exists to catch.
if grep -qa 'Table Branch: Ingress.lock_tbl | Key(s): 0' "$tmp"/*.txtpb; then
    echo "FAIL [zero-regression]: a Phase-1 HIT on key 0 means the declared init was ignored"
    fail=1
else
    echo "PASS [zero-regression]: no Phase-1 HIT on key 0"
fi

echo
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT"
exit "$fail"
