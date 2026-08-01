#!/usr/bin/env bash
# Commit-4 evidence gate: fixed-budget ablation of the tampering-search options.
#
# Do NOT measure time-to-completion — the baseline does not complete, so it has no such number.
# Instead fix the budget (default 1h) and compare PROGRESS: chains completed and txtpb emitted,
# across four configurations on the four bmv2/v1model programs that cannot finish in 3h.
#
#   none    --shared-traversal=NONE            cost of NOT having Phase-1 batching   (commit 1)
#   p1      --shared-traversal=PHASE1          reference / today's default
#   p12     --shared-traversal=PHASE1_PHASE2   + global Phase-2 write-path prefilter (commit 2)
#   p12ord  p12 + --chain-impact-order          + class-A chain ordering              (commit 3)
#
# Recorded PHASE1 reference (from the plan): Pegasus 11/52, Blink 23/53, SISTAR 6/15, netchain 1/4.
#
# The two new capabilities improve DIFFERENT metrics — the prefilter raises chains-completed, the
# ordering raises findings-per-chain — so both columns are reported or commit 3 looks like a no-op.
#
# Scope: generation only. "VULNERABLE" counts need a bmv2 replay pass (tampering.py --stage run) and
# are NOT measured here; the txtpb column is the generation-side proxy.
#
# Usage:
#   bash search_opt_ablation.sh            # run the matrix (JOBS=4, BUDGET=3600)
#   JOBS=8 bash search_opt_ablation.sh     # 8 concurrent runs (~2 waves)
#   bash search_opt_ablation.sh summarize  # re-print the table from existing logs
#
# Resumable: a cell whose .done marker exists is skipped, so an interrupted matrix can be re-run.
set -uo pipefail

SX="${P4SYMBEX:-$HOME/Workspace/p4c/build/p4symbex}"
TR="${TOP_TIER:-$HOME/Workspace-remote/top_tier_repo}"
CACHE="${CACHE_ROOT:-$HOME/Workspace-remote/p4symbex_nightly/sochain_cache}"
# Persistent (survives reboot) — never /tmp.
OUT="${ABL_OUT:-$HOME/Workspace-remote/p4symbex_nightly/ablation}"
BUDGET="${BUDGET:-3600}"
JOBS="${JOBS:-4}"
MAXTESTS="${MAXTESTS:-4}"

# name|source|std   (all bmv2/v1model; netchain is p4-14)
PROGRAMS=(
    "Pegasus-osdi20_p4_bmv2_pegasus|$TR/Pegasus-osdi20/p4/bmv2/pegasus.p4|16"
    "Blink_p4_code_main|$TR/Blink/p4_code/main.p4|16"
    "SISTAR_BMv2_DT|$TR/SISTAR/BMv2/DT.p4|16"
    "netchain-p4_p4src_netchain|$TR/netchain-p4/p4src/netchain.p4|14"
)
# label|extra p4symbex args
CONFIGS=(
    "none|--shared-traversal NONE"
    "p1|--shared-traversal PHASE1"
    "p12|--shared-traversal PHASE1_PHASE2"
    "p12ord|--shared-traversal PHASE1_PHASE2 --chain-impact-order"
)

run_cell() {
    # args: name|src|std|cfg|cfgargs
    IFS='|' read -r name src std cfg cfgargs <<<"$1"
    local dir="$OUT/$cfg/$name" log="$OUT/$cfg/$name.log" done="$OUT/$cfg/$name.done"
    if [ -f "$done" ]; then echo "  skip (done): $cfg/$name"; return 0; fi
    mkdir -p "$dir"
    local cachefile="$CACHE/bmv2/v1model/$name.chains"
    if [ ! -f "$cachefile" ]; then echo "  MISSING CACHE: $cachefile" | tee "$log"; return 1; fi
    echo "  start $cfg/$name ($(date +%T))"
    local t0 t1
    t0=$(date +%s)
    # shellcheck disable=SC2086
    timeout "$BUDGET" "$SX" \
        --target bmv2 --arch v1model --std "$std" \
        --test-backend protobuf --packet-size-range 14:9600 --track-coverage STATEMENTS \
        --max-tests "$MAXTESTS" --max-port 8 \
        --state-dep --path-selection STATE_DEP_TAMPERING_COND \
        --state-dep-cache "$cachefile" \
        $cfgargs --out-dir "$dir" "$src" >"$log" 2>&1
    local rc=$?
    t1=$(date +%s)
    echo "rc=$rc elapsed=$((t1 - t0))" >"$done"
    echo "  end   $cfg/$name rc=$rc ($((t1 - t0))s)"
}
export -f run_cell
export SX TR CACHE OUT BUDGET MAXTESTS

summarize() {
    local csv="$OUT/ablation_results.csv"
    echo "config,program,rc,elapsed_s,chains_done,chains_total,txtpb,pruned,impact_ranked" >"$csv"
    for p in "${PROGRAMS[@]}"; do
        IFS='|' read -r name _ _ <<<"$p"
        for c in "${CONFIGS[@]}"; do
            IFS='|' read -r cfg _ <<<"$c"
            local log="$OUT/$cfg/$name.log" done="$OUT/$cfg/$name.done" dir="$OUT/$cfg/$name"
            [ -f "$log" ] || continue
            local rc elapsed cd ct tx pr ir
            rc=$(sed -E 's/rc=([0-9]+).*/\1/' "$done" 2>/dev/null); rc=${rc:-NA}
            elapsed=$(sed -E 's/.*elapsed=([0-9]+)/\1/' "$done" 2>/dev/null); elapsed=${elapsed:-NA}
            cd=$(grep -o 'Chains completed: [0-9]*' "$log" | tail -1 | grep -oE '[0-9]+$'); cd=${cd:-0}
            ct=$(grep -o 'Chains completed: [0-9]*/[0-9]*' "$log" | tail -1 | grep -oE '[0-9]+$'); ct=${ct:-NA}
            tx=$(find "$dir" -name '*.txtpb' 2>/dev/null | wc -l | tr -d ' ')
            pr=$(grep -o 'prefilter: [0-9]* chains, [0-9]* pruned' "$log" | tail -1 | grep -oE '[0-9]+ pruned' | grep -oE '^[0-9]+'); pr=${pr:--}
            ir=$(grep -o 'Chain order: [0-9]*' "$log" | tail -1 | grep -oE '[0-9]+$'); ir=${ir:--}
            echo "$cfg,$name,$rc,$elapsed,$cd,$ct,$tx,$pr,$ir" >>"$csv"
        done
    done
    echo
    printf '%-8s %-32s %6s %9s %8s %7s %8s\n' CONFIG PROGRAM RC ELAPSED CHAINS TXTPB PRUNED
    tail -n +2 "$csv" | while IFS=, read -r cfg name rc el cd ct tx pr ir; do
        printf '%-8s %-32s %6s %9s %8s %7s %8s\n' "$cfg" "${name:0:32}" "$rc" "$el" "$cd/$ct" "$tx" "$pr"
    done
    echo
    echo "csv: $csv"
}

if [ "${1:-run}" = "summarize" ]; then summarize; exit 0; fi

[ -x "$SX" ] || { echo "p4symbex not found/executable: $SX" >&2; exit 1; }
mkdir -p "$OUT"
jobfile="$OUT/.jobs"; : >"$jobfile"
for p in "${PROGRAMS[@]}"; do
    for c in "${CONFIGS[@]}"; do
        IFS='|' read -r cfg cfgargs <<<"$c"
        echo "$p|$cfg|$cfgargs" >>"$jobfile"
    done
done

echo "=== ablation: $(wc -l <"$jobfile") cells, JOBS=$JOBS, BUDGET=${BUDGET}s -> $OUT ==="
echo "=== started $(date) ==="
xargs -a "$jobfile" -d '\n' -P "$JOBS" -I{} bash -c 'run_cell "$@"' _ {}
echo "=== finished $(date) ==="
summarize
