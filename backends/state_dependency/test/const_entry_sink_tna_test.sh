#!/usr/bin/env bash
# Const-entry sink coverage regression
# (core/symbolic_executor/state_dependency_track.cpp::constEntriesCoverKeySpace + its use in
#  runTamperingChain).
#
# A table whose `const entries` cover its whole key space can never MISS, so a HIT<->MISS tamper
# against it is structurally unreachable and every emitted case is a false positive. linkguardian's
# `decide_retx_or_drop` is the real instance: 5 one-bit ternary keys covered by masked cubes
# ((1,_,_,_,_) ... plus (0,0,0,0,0)). It produced 8 h2m cases that can never reproduce.
#
# Two fixtures, because the dangerous failure mode is over-suppression -- a check that answers
# "covered" for any table with const entries would silently delete real findings and still look green:
#   const_entry_sink_tna.p4     covered   -> suppression MUST fire, nothing emitted
#   const_entry_partial_tna.p4  (1,0) unnamed -> suppression MUST NOT fire
#
# A third arm runs the covered fixture again with --const-entry-action-divergence. "Can never MISS"
# and "nothing can change here" are different claims, and only the first is true of this sink: its
# const map selects fwd() for one key and drop() for another, so moving the key IS observable. That
# arm asserts the additive pass finds it, and that the emitted case describes itself honestly.
#
# Generation-only (no Tofino HW replay on this machine).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
INC=(-I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include")
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

run_one() {   # $1 = fixture stem, $2... = extra p4symbex flags (output dir is keyed on $1)
    local stem="$1"; shift
    local out="$tmp/$stem" log="$tmp/$stem.log"
    mkdir -p "$out"
    timeout 900 "$SX" --target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 "${INC[@]}" \
        --test-backend BFRT --packet-size-range 14:9600 --track-coverage STATEMENTS \
        --max-tests 4 --max-port 32 --out-dir "$out" \
        --state-dep --path-selection STATE_DEP_TAMPERING "$@" "$HERE/${stem%%@*}.p4" \
        --state-tamper-value 0x1 > "$log" 2>&1
    echo $?
}

fail=0

rc=$(run_one const_entry_sink_tna)
n=$(find "$tmp/const_entry_sink_tna" -name '*.txtpb' | wc -l | tr -d ' ')
sup=$(grep -c 'can never MISS' "$tmp/const_entry_sink_tna.log")
if [ "$rc" -ne 0 ]; then
    echo "FAIL [covered]: p4symbex exit $rc"; tail -5 "$tmp/const_entry_sink_tna.log" | sed 's/^/    /'; fail=1
elif [ "$sup" -lt 1 ]; then
    echo "FAIL [covered]: suppression did not fire -- full-coverage sink was not recognized"; fail=1
elif [ "$n" -ne 0 ]; then
    echo "FAIL [covered]: $n txtpb emitted against a sink that can never MISS"; fail=1
else
    echo "PASS [covered]: suppressed (x$sup), 0 txtpb"
fi

rc=$(run_one const_entry_partial_tna)
sup=$(grep -c 'can never MISS' "$tmp/const_entry_partial_tna.log")
n=$(find "$tmp/const_entry_partial_tna" -name '*.txtpb' | wc -l | tr -d ' ')
if [ "$rc" -ne 0 ]; then
    echo "FAIL [partial]: p4symbex exit $rc"; tail -5 "$tmp/const_entry_partial_tna.log" | sed 's/^/    /'; fail=1
elif [ "$sup" -ne 0 ]; then
    echo "FAIL [partial]: suppression fired on a sink that CAN miss -- over-suppression"; fail=1
else
    echo "PASS [partial]: not suppressed ($n txtpb emitted)"
fi

# Arm 3: the same fully-covered sink WITH --const-entry-action-divergence. The suppression above is
# only correct about HIT/MISS -- moving the key from `fwd()` to `drop()` is a real, program-fixed
# divergence, and this arm is what keeps the two claims from being confused. It also guards the
# emission shape: an action divergence reaches the sink in BOTH replays, so it must name its two
# outcomes and must NOT carry a hit/miss phase pair, which would be a fiction the harness lints
# against the installed keys.
rc=$(run_one 'const_entry_sink_tna@adiv' --const-entry-action-divergence)
advdir="$tmp/const_entry_sink_tna@adiv"
n=$(find "$advdir" -name '*.txtpb' | wc -l | tr -d ' ')
tagged=$(find "$advdir" -name '*_adiv.txtpb' | wc -l | tr -d ' ')
legit=$(grep -h 'sink_outcome_legit:' "$advdir"/*.txtpb 2>/dev/null | sort -u | wc -l | tr -d ' ')
attack=$(grep -h 'sink_outcome_attack:' "$advdir"/*.txtpb 2>/dev/null | sort -u | wc -l | tr -d ' ')
same=$(paste -d'|' \
        <(grep -ho 'sink_outcome_legit: ".*"' "$advdir"/*.txtpb 2>/dev/null | sed 's/.*: //') \
        <(grep -ho 'sink_outcome_attack: ".*"' "$advdir"/*.txtpb 2>/dev/null | sed 's/.*: //') \
      | awk -F'|' '$1 == $2' | wc -l | tr -d ' ')
phases=$(grep -c 'hit_phase\|miss_phase' "$advdir"/*.txtpb 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')
if [ "$rc" -ne 0 ]; then
    echo "FAIL [adiv]: p4symbex exit $rc"; tail -5 "$tmp/const_entry_sink_tna@adiv.log" | sed 's/^/    /'; fail=1
elif [ "$n" -lt 1 ]; then
    echo "FAIL [adiv]: 0 txtpb -- the const map selects fwd() vs drop(), so a key movement inside it IS a divergence"; fail=1
elif [ "$tagged" -ne "$n" ]; then
    echo "FAIL [adiv]: $tagged/$n files carry the _adiv kind tag"; fail=1
elif [ "$legit" -lt 1 ] || [ "$attack" -lt 1 ]; then
    echo "FAIL [adiv]: emitted case does not name both sink outcomes"; fail=1
elif [ "$same" -ne 0 ]; then
    echo "FAIL [adiv]: $same case(s) report an IDENTICAL legit/attack outcome -- the criterion is not reflexive"; fail=1
elif [ "$phases" -ne 0 ]; then
    echo "FAIL [adiv]: $phases hit_phase/miss_phase line(s) emitted; an action divergence reaches the sink in both runs"; fail=1
else
    echo "PASS [adiv]: $n txtpb, all _adiv, distinct outcome pairs, no hit/miss phase"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
