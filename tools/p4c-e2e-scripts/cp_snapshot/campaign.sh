#!/usr/bin/env bash
# campaign.sh <outdir> [program ...]
# Capturable-H2S2 generation: every program that has a --cp-snapshot, for each output-chain category
# (OUTK, OUTC, O) in which its nightly chain cache has chains, with --diverge-output PORT. One job
# per (program, category), at most JOBS (10) at a time, 32GB and TIMEOUT (1800s) each.
set -uo pipefail
OUT="$1"; shift
R=/root/jiwon/Workspace/p4c
SNAPS=/root/jiwon/Workspace-remote/sp27/cp_snapshots
LIST=/root/jiwon/Workspace-remote/usenix27/program_list_151_no_oom.txt
CACHE=/root/jiwon/Workspace-remote/sp27/p4symbex_nightly/sochain_cache
SX="${P4SYMBEX:-$R/build/p4symbex}"
programs=("$@"); [ ${#programs[@]} -eq 0 ] && programs=($(ls $SNAPS/*.txtpb | xargs -n1 basename | sed 's/\.txtpb$//'))
mkdir -p "$OUT"
jobs_file="$OUT/jobs.txt"; : > "$jobs_file"
for P in "${programs[@]}"; do
  read -r _ p4 tgt arch _ < <(grep -E "^$P " "$LIST" | sed 's#~/#/root/#g')
  cache="$CACHE/$tgt/$arch/$P.chains"
  for cat in h2s2outk:OUTK:STATE_DEP_TAMPERING h2s2outc:OUTC:STATE_DEP_TAMPERING_COND h2s2o:O:STATE_DEP_TAMPERING; do
    IFS=: read -r key sinks pol <<< "$cat"
    n=$(python3 -I -c "import json,sys; c=json.load(open(sys.argv[1])); print(sum(len(v) for v in c.get(sys.argv[2],{}).values()))" "$cache" "$key")
    [ "$n" -gt 0 ] && echo "$P $tgt $arch $p4 $cache $sinks $pol $n" >> "$jobs_file"
  done
done
run_job() {
  read -r P tgt arch p4 cache sinks pol n <<< "$1"
  d="$OUT/$P/$sinks"; rm -rf "$d"; mkdir -p "$d"
  if [ "$tgt" = tofino ]; then
    T=(--target tofino --arch "$arch" --std p4-16 -D__TARGET_TOFINO__=1
       -I$R/backends/tofino/bf-p4c/p4include -I$R/p4include --test-backend BFRT --max-port 32)
  else
    T=(--target bmv2 --arch v1model --std p4-16 -I$R/p4include --test-backend protobuf --max-port 32)
  fi
  ( cd "$d" && HOME=/root/jiwon prlimit --as=34359738368 timeout "${TIMEOUT:-1800}" "$SX" "${T[@]}" \
      --packet-size-range 14:9600 --track-coverage STATEMENTS --max-tests "${MAX_TESTS:-8}" \
      --out-dir "$d" --state-dep --path-selection "$pol" --state-dep-sinks "$sinks" \
      --diverge-output PORT --state-dep-cache "$cache" \
      --cp-snapshot "/root/jiwon/Workspace-remote/sp27/cp_snapshots/$P.txtpb" "$p4" \
      --state-tamper-value 0xdeadbeef > "$d/p4symbex.log" 2>&1 )
  echo "$P $sinks chains=$n rc=$? $(grep -h 'Symbex Summary' "$d/p4symbex.log" | tail -1 | grep -o 'tests=[0-9]* status=[a-z]*') files=$(ls "$d"/*.txtpb 2>/dev/null | wc -l)"
}
export -f run_job; export R SX OUT
wc -l < "$jobs_file"
xargs -a "$jobs_file" -d '\n' -P "${JOBS:-10}" -I{} bash -c 'run_job "$@"' _ {} | tee "$OUT/summary.txt"
