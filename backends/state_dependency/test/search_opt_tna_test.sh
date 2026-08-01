#!/usr/bin/env bash
# Search-optimization regression for --shared-traversal (and later --chain-impact-order).
#
# Fixture: search_opt_tna.p4 — four H2S2C chains (regA..regD) with all four reads on ONE path and
# regD's WRITE guarded by a contradiction (v == 1 && v == 2).
#
# The load-bearing claim is that these options change WORK DONE, never RESULTS. So the primary
# assertion is that every mode emits the SAME tests; the metrics lines are what must differ.
#
#   --shared-traversal=NONE          per-chain Phase-1 passes (baseline; one per chain)
#   --shared-traversal=PHASE1        one shared Phase-1 pass, ~4 buckets per terminal   [default]
#   --shared-traversal=PHASE1_PHASE2 additionally prunes regD before the constrained loop
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
COMMON=(--target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}"
        --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS
        --max-tests 4 --max-port 32 --state-dep --path-selection STATE_DEP_TAMPERING_COND)
P4="$HERE/search_opt_tna.p4"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

# gen <mode|-> <outdir> <logfile> [extra args...]
gen() {
    local mode="$1" out="$2" log="$3"; shift 3
    mkdir -p "$out"
    local extra=()
    [ "$mode" != "-" ] && extra=(--shared-traversal "$mode")
    timeout 900 "$SX" "${COMMON[@]}" "${extra[@]}" --out-dir "$out" "$P4" "$@" > "$log" 2>&1
}

# Canonical fingerprint of the emitted tests: names + affected_register triples, order-insensitive.
fingerprint() {
    local out="$1"
    { find "$out" -name '*.txtpb' -printf '%f\n' | sort
      grep -rho -E 'register_name: "[^"]*"|index: [0-9]+|attacker_value: "[^"]*"' \
           "$out" --include='*.txtpb' 2>/dev/null | sort; } | md5sum | cut -d' ' -f1
}

echo "=== generating: NONE / PHASE1 / PHASE1_PHASE2 ==="
gen NONE          "$tmp/none" "$tmp/none.log"
gen PHASE1        "$tmp/p1"   "$tmp/p1.log"
gen PHASE1_PHASE2 "$tmp/p12"  "$tmp/p12.log"
gen -             "$tmp/def"  "$tmp/def.log"

n_none=$(find "$tmp/none" -name '*.txtpb' | wc -l | tr -d ' ')
n_p1=$(find "$tmp/p1"   -name '*.txtpb' | wc -l | tr -d ' ')
n_p12=$(find "$tmp/p12" -name '*.txtpb' | wc -l | tr -d ' ')
n_def=$(find "$tmp/def" -name '*.txtpb' | wc -l | tr -d ' ')

# ---- 0. the fixture must actually yield tests, else nothing below is meaningful ----
if [ "$n_p1" -eq 0 ]; then
    echo "FAIL [fixture]: PHASE1 emitted no txtpb — chains did not form/yield"
    grep -E "no chains|no terminal|Unable to generate|Symbex Bug" "$tmp/p1.log" | head -3 | sed 's/^/    /'
    fail=1
fi

# ---- 1. RESULT PARITY: the options must not change what is emitted ----
f_none=$(fingerprint "$tmp/none"); f_p1=$(fingerprint "$tmp/p1")
f_p12=$(fingerprint "$tmp/p12");   f_def=$(fingerprint "$tmp/def")
if [ "$n_none" -ne "$n_p1" ] || [ "$f_none" != "$f_p1" ]; then
    echo "FAIL [parity NONE vs PHASE1]: $n_none vs $n_p1 txtpb, fingerprint $f_none vs $f_p1"
    echo "       the traversal scope changed RESULTS, not just work — this is the correctness claim"
    fail=1
else
    echo "PASS [parity NONE vs PHASE1]: $n_p1 txtpb, identical fingerprint"
fi
if [ "$n_p12" -ne "$n_p1" ] || [ "$f_p12" != "$f_p1" ]; then
    echo "FAIL [parity PHASE1 vs PHASE1_PHASE2]: $n_p1 vs $n_p12 txtpb, fp $f_p1 vs $f_p12"
    echo "       the Phase-2 prefilter must only PRUNE infeasible chains, never drop real tests"
    fail=1
else
    echo "PASS [parity PHASE1 vs PHASE1_PHASE2]: $n_p12 txtpb, identical fingerprint"
fi
if [ "$f_def" != "$f_p1" ]; then
    echo "FAIL [default]: no-option run differs from --shared-traversal=PHASE1 (fp $f_def vs $f_p1)"
    fail=1
else
    echo "PASS [default]: no-option run == PHASE1"
fi

# ---- 2. COMMIT 1: shared pass buckets one terminal into several chains; NONE runs per chain ----
shared_line=$(grep -o 'Shared Phase-1: .*' "$tmp/p1.log" | head -1)
per_chain_n=$(grep -c 'Per-chain Phase-1: chain id=' "$tmp/none.log")
shared_in_none=$(grep -c 'Shared Phase-1:' "$tmp/none.log")
examined=$(sed -E 's/.*Shared Phase-1: [0-9]+ chains, ([0-9]+) terminals examined.*/\1/' <<<"$shared_line")
bucketed=$(sed -E 's/.*terminals examined, ([0-9]+) bucketed.*/\1/' <<<"$shared_line")
if [ -z "$shared_line" ]; then
    echo "FAIL [commit1]: PHASE1 printed no 'Shared Phase-1:' metric line"; fail=1
elif [ "$shared_in_none" -ne 0 ]; then
    echo "FAIL [commit1]: NONE still ran the shared pass ($shared_in_none times)"; fail=1
elif [ "$per_chain_n" -lt 2 ]; then
    echo "FAIL [commit1]: NONE printed only $per_chain_n per-chain Phase-1 passes (expected one per chain)"
    fail=1
elif ! [ "$bucketed" -gt "$examined" ] 2>/dev/null; then
    echo "FAIL [commit1]: shared pass bucketed=$bucketed !> examined=$examined — no sharing observed"
    echo "       (all four reads are on one path, so each terminal should land in several buckets)"
    fail=1
else
    echo "PASS [commit1]: shared bucketed=$bucketed from examined=$examined; NONE ran $per_chain_n per-chain passes"
fi

# ---- 3. COMMIT 2: the prefilter must prune regD (write guarded by a contradiction) ----
pre_line=$(grep -o 'Phase-2 prefilter: .*' "$tmp/p12.log" | head -1)
if [ -z "$pre_line" ]; then
    echo "SKIP [commit2]: no 'Phase-2 prefilter:' line yet (not implemented until commit 2)"
else
    pruned=$(sed -E 's/.*chains, ([0-9]+) pruned.*/\1/' <<<"$pre_line")
    if ! [ "$pruned" -ge 1 ] 2>/dev/null; then
        echo "FAIL [commit2]: prefilter pruned $pruned chains; regD's write is unreachable so >=1 expected"
        fail=1
    else
        echo "PASS [commit2]: $pre_line"
    fi
fi

# ---- 4. COMMIT 3: exactly one class-A chain (sink A applies drop_tbl -> drop_ctl) ----
# --chain-impact-order is a separate opt-in boolean, so it needs its own run.
gen PHASE1_PHASE2 "$tmp/ord" "$tmp/ord.log" --chain-impact-order
n_ord=$(find "$tmp/ord" -name '*.txtpb' | wc -l | tr -d ' ')
f_ord=$(fingerprint "$tmp/ord")
ord_line=$(grep -o 'Chain order: .*' "$tmp/ord.log" | head -1)
if [ -z "$ord_line" ]; then
    echo "SKIP [commit3]: no 'Chain order:' line yet (not implemented until commit 3)"
else
    classa=$(sed -E 's/.*Chain order: ([0-9]+) impact-ranked.*/\1/' <<<"$ord_line")
    # Results must be identical with and without the ordering (it reorders work, never results).
    if [ "$n_ord" -ne "$n_p12" ] || [ "$f_ord" != "$f_p12" ]; then
        echo "FAIL [parity --chain-impact-order]: $n_p12 vs $n_ord txtpb, fp $f_p12 vs $f_ord"
        echo "       ordering must not change WHICH tests are emitted, only their order"
        fail=1
    elif ! [ "$classa" -eq 1 ] 2>/dev/null; then
        echo "FAIL [commit3]: $classa chains impact-ranked; exactly 1 expected (only regA's sink drops)"
        fail=1
    else
        echo "PASS [commit3]: $ord_line (results identical to PHASE1_PHASE2)"
    fi
fi

# ---- 5. the unconditional progress metric must be present in every mode ----
for m in none p1 p12 def; do
    if ! grep -q 'Chains completed:' "$tmp/$m.log"; then
        echo "FAIL [metric]: '$m' run printed no 'Chains completed:' line"; fail=1
    fi
done

echo
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT"
exit "$fail"
