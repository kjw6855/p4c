#!/usr/bin/env bash
# --cp-snapshot regression (core/symbolic_executor/cp_snapshot.cpp and its hooks).
#
# The fixtures cp_snapshot_{tna,v1model}.p4 are built so that ONLY the snapshot makes the
# state_reg -> sink_tbl chain exploitable: cfg's runtime default opens the stateful path, sink_tbl
# forwards only the installed key, and the controller-written state_reg[5] is what lets a victim HIT.
# Each arm removes one of those facts and checks the consequence:
#   full      HIT->MISS cases; victim on a HOST port, attacker on the ATTACKER port; the snapshot is
#             emitted once (snapshot_entities) and no per-phase entry is synthesized (closed world)
#   noreg     without the register cell the victim reads 0 and MISSes: no HIT->MISS case
#   nodefault without cfg's runtime default the stateful path is closed: no case at all
# The full arm also checks the register-knowledge tag: the tna attacker must match gate_reg[0],
# which only the controller sets, so its case needs knowledge; the v1model attacker does not.
# plus option errors (with --cp-annotation, without a tampering policy, two snapshots, a bad action).
#
# Generation-only (no target replay).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SX="${P4SYMBEX:-$ROOT/build/p4symbex}"
BM2="${P4C_BM2_SS:-$ROOT/build/backends/bmv2/p4c-bm2-ss}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

TNA=(--target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1
     -I "$ROOT/backends/tofino/bf-p4c/p4include" -I "$ROOT/p4include" --test-backend BFRT)
BMV2=(--target bmv2 --arch v1model --std p4-16 -I "$ROOT/p4include" --test-backend protobuf)

run_one() {   # $1 = output tag, $2 = p4 stem, $3 = target (tna|v1model), $4... = extra flags
    local tag="$1" stem="$2" tgt="$3"; shift 3
    local out="$tmp/$tag"
    local targs=("${TNA[@]}"); [ "$tgt" = v1model ] && targs=("${BMV2[@]}")
    mkdir -p "$out"
    (cd "$out" && timeout 900 "$SX" "${targs[@]}" --packet-size-range 14:9600 \
        --track-coverage STATEMENTS --max-tests 4 --max-port 32 --out-dir "$out" --state-dep "$@" \
        "$HERE/$stem.p4" --state-tamper-value 0x1 > "$tmp/$tag.log" 2>&1)
    echo $?
}

# Phase ports and emission shape of every case in $1, as "<cases> <h2m> <bad>" where bad counts
# cases with a per-phase entry, no snapshot block, a victim off {1,2} or an attacker off {3}.
check_cases() {
    python3 -I - "$1" <<'PY'
import glob, re, sys
cases = sorted(glob.glob(sys.argv[1] + '/*.txtpb'))
bad = 0
for f in cases:
    text = open(f).read()
    blocks = re.findall(r'^input_packet \{(.*?)^\}', text, re.S | re.M)
    ports = [int(re.search(r'port: (\d+)', b).group(1)) for b in blocks]
    tamper = [('tamper_only: true' in b) for b in blocks]
    if not any(tamper) and len(blocks) == 3:
        tamper = [False, True, False]          # bmv2: Phase 2 is the second packet
    attacker = [p for p, t in zip(ports, tamper) if t]
    victim = [p for p, t in zip(ports, tamper) if not t]
    ok = (re.search(r'^entities \{', text, re.M) is None
          and text.count('snapshot_entities {') >= 3
          and attacker and set(attacker) <= {3} and victim and set(victim) <= {1, 2})
    if not ok:
        bad += 1
        print('  bad case:', f.split('/')[-1], 'victim', victim, 'attacker', attacker, file=sys.stderr)
print(len(cases), sum(1 for f in cases if f.endswith('_h2m.txtpb')), bad)
PY
}

rules_ok() {   # every expected rule line of $1 is in the full arm's log
    local line
    while IFS= read -r line; do
        grep -qF -- "[CP snapshot] ${line%% -> *} -> " "$tmp/$1.full.log" || { echo "    missing: $line"; return 1; }
        grep -F -- "[CP snapshot] ${line%% -> *} -> " "$tmp/$1.full.log" | grep -q -- "${line##* -> }" \
            || { echo "    wrong action: $line"; return 1; }
    done <<< "${rules[$1]}"
}

fail=0
pass() { echo "PASS [$1]: $2"; }
failed() { echo "FAIL [$1]: $2"; [ -n "${3:-}" ] && tail -5 "$tmp/$3.log" | sed 's/^/    /'; fail=1; }

# Snapshot variants. The bmv2 register entry names its register by P4Info id.
"$BM2" -I "$ROOT/p4include" --p4runtime-files "$tmp/v1.p4info.txtpb" -o "$tmp/v1.json" \
    "$HERE/cp_snapshot_v1model.p4" > "$tmp/bm2.log" 2>&1 || { echo "FAIL: p4c-bm2-ss"; exit 1; }
regid=$(grep -A3 '^registers' "$tmp/v1.p4info.txtpb" | awk '/id:/ {print $2; exit}')
sed "s/REGISTER_ID/$regid/" "$HERE/cp_snapshot_v1model.txtpb" > "$tmp/v1model.txtpb"
cp "$HERE/cp_snapshot_tna.txtpb" "$tmp/tna.txtpb"
for t in tna v1model; do
    # noreg: drop the register entity; nodefault: drop cfg's default entry.
    python3 -I - "$tmp/$t.txtpb" "$tmp/$t.noreg.txtpb" "$tmp/$t.nodefault.txtpb" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
blocks = re.split(r'(?m)^(?=(?:bfrt_)?entities \{)', text)
open(sys.argv[2], 'w').write(''.join(b for b in blocks if 'REGISTER_INDEX' not in b and 'register_entry' not in b))
open(sys.argv[3], 'w').write(''.join(b for b in blocks if '.cfg"' not in b))
PY
done

# The tna attacker's write is gated on a controller-set cell (gate_reg[0]); the v1model one is not.
declare -A knowledge=([tna]="required: Ingress.gate_reg_0" [v1model]="none")
declare -A closed=([tna]="4 tables with 5 snapshot entries, 1 tables empty"
                   [v1model]="2 tables with 2 snapshot entries, 1 tables empty")
# Materialized rules, in evaluation order: BfRt's smaller priority first, a full ternary mask as a
# constant, the $valid key resolved, LPM as a prefix mask, range bounds; an omitted optional key is
# a wildcard. Each line must appear in the full arm's log.
declare -A rules=([tna]='Ingress.pri_tbl #0: {0 &&& 15, 1}; -> NoAction
Ingress.pri_tbl #1: {64, 1}; -> mark
Ingress.lpm_tbl #0: {167772160 &&& 4278190080}; -> mark
Ingress.rng_tbl #0: {100 .. 200}; -> mark'
                  [v1model]='MyIngress.opt_tbl #0: {1, default}; -> mark')
for t in tna v1model; do
    stem=cp_snapshot_$t
    pol=(--path-selection STATE_DEP_TAMPERING)
    rc=$(run_one "$t.full" "$stem" "$t" "${pol[@]}" --cp-snapshot "$tmp/$t.txtpb")
    read -r n h2m bad < <(check_cases "$tmp/$t.full")
    if [ "$rc" -ne 0 ]; then failed "$t full" "p4symbex exit $rc" "$t.full"
    elif [ "$h2m" -lt 1 ]; then failed "$t full" "no HIT->MISS case ($n cases)" "$t.full"
    elif [ "$bad" -ne 0 ]; then failed "$t full" "$bad case(s) with a per-phase entry, no snapshot block or a wrong port role"
    elif ! grep -q "closed world: ${closed[$t]}" "$tmp/$t.full.log"; then
        failed "$t full" "closed-world summary line missing or wrong" "$t.full"
    elif grep -q 'matches no table' "$tmp/$t.full.log"; then
        failed "$t full" "a snapshot table (e.g. \$mirror.cfg/\$pre.*) was reported as unmatched" "$t.full"
    elif ! rules_ok "$t"; then
        failed "$t full" "materialized rules differ from the expected order/forms" "$t.full"
    elif ! grep -qh "Register knowledge: ${knowledge[$t]}" "$tmp/$t.full"/*.txtpb; then
        failed "$t full" "expected 'Register knowledge: ${knowledge[$t]}' in the cases" "$t.full"
    else pass "$t full" "$n cases ($h2m h2m), ports, emission and register knowledge as expected"; fi

    rc=$(run_one "$t.noreg" "$stem" "$t" "${pol[@]}" --cp-snapshot "$tmp/$t.noreg.txtpb")
    read -r n h2m bad < <(check_cases "$tmp/$t.noreg")
    if [ "$rc" -ne 0 ]; then failed "$t noreg" "p4symbex exit $rc" "$t.noreg"
    elif [ "$h2m" -ne 0 ]; then failed "$t noreg" "$h2m HIT->MISS case(s) although the victim reads 0 and MISSes"
    else pass "$t noreg" "no HIT->MISS case without the controller-written cell ($n other)"; fi

    # initial: the full snapshot, but the search starts from declared initial values (the
    # attacker's view). Like noreg the victim then MISSes; any case still carries the deployed cell
    # in its snapshot block, and is tagged "initial".
    rc=$(run_one "$t.initial" "$stem" "$t" "${pol[@]}" --cp-snapshot "$tmp/$t.txtpb" \
        --cp-snapshot-registers INITIAL)
    read -r n h2m bad < <(check_cases "$tmp/$t.initial")
    if [ "$rc" -ne 0 ]; then failed "$t initial" "p4symbex exit $rc" "$t.initial"
    elif [ "$h2m" -ne 0 ]; then failed "$t initial" "$h2m HIT->MISS case(s) although the search reads the declared 0"
    elif [ "$n" -gt 0 ] && ! grep -qlE 'REGISTER_INDEX|register_entry' "$tmp/$t.initial"/*.txtpb; then
        failed "$t initial" "cases lost the deployed register cell from the snapshot block"
    elif [ "$n" -gt 0 ] && [ -n "$(grep -L 'Register knowledge: initial' "$tmp/$t.initial"/*.txtpb)" ]; then
        failed "$t initial" "cases are not tagged 'Register knowledge: initial'"
    else pass "$t initial" "no HIT->MISS case from declared initial values ($n other)"; fi

    rc=$(run_one "$t.nodefault" "$stem" "$t" "${pol[@]}" --cp-snapshot "$tmp/$t.nodefault.txtpb")
    read -r n h2m bad < <(check_cases "$tmp/$t.nodefault")
    if [ "$rc" -ne 0 ]; then failed "$t nodefault" "p4symbex exit $rc" "$t.nodefault"
    elif [ "$n" -ne 0 ]; then failed "$t nodefault" "$n case(s) although cfg's P4 default keeps the stateful path closed"
    else pass "$t nodefault" "no case without cfg's runtime default"; fi
done

# Option errors: each must fail with its message.
expect_error() {   # $1 = tag, $2 = expected message, $3... = flags
    local tag="$1" msg="$2"; shift 2
    local rc; rc=$(run_one "$tag" cp_snapshot_tna tna "$@")
    if [ "$rc" -eq 0 ] || ! grep -q -- "$msg" "$tmp/$tag.log"; then failed "$tag" "expected an error: $msg" "$tag"
    else pass "$tag" "rejected"; fi
}
expect_error err.annotation "mutually exclusive" --path-selection STATE_DEP_TAMPERING \
    --cp-snapshot "$tmp/tna.txtpb" --cp-annotation "$HERE/cp_annotation_port.json"
expect_error err.policy "requires --path-selection" --cp-snapshot "$tmp/tna.txtpb"
expect_error err.two "multiple snapshots" --path-selection STATE_DEP_TAMPERING \
    --cp-snapshot "$tmp/tna.txtpb" --cp-snapshot "$tmp/tna.txtpb"
expect_error err.registers "requires --cp-snapshot" --path-selection STATE_DEP_TAMPERING \
    --cp-snapshot-registers INITIAL
sed 's/"Ingress.fwd"/"Ingress.no_such_action"/' "$tmp/tna.txtpb" > "$tmp/tna.badaction.txtpb"
expect_error err.action "has no action" --path-selection STATE_DEP_TAMPERING \
    --cp-snapshot "$tmp/tna.badaction.txtpb"

echo
if [ "$fail" -eq 0 ]; then echo "ALL PASS"; exit 0; fi
echo "FAILURES above"; exit 1
