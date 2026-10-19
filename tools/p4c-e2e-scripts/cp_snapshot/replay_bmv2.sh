#!/usr/bin/env bash
# replay_bmv2.sh <campaign_dir> [program ...]
# Replays a cp-snapshot campaign's bmv2 cases with tampering.py --stage run inside the
# p4lang/behavioral-model container (simple_switch_grpc, P4Runtime, scapy and tcpdump live there),
# on host veths (--net=host). The program is compiled on the host with p4c-bm2-ss, so the run stage
# finds its JSON/P4Info and skips compilation; snapshot_ids.py rewrites the cases' snapshot blocks
# from the p4runtime fork's names to P4Info ids for the container's upstream P4Runtime. Verdicts: <campaign_dir>/_replay_bmv2/<cat>/results.csv.
set -uo pipefail
CAMP="$(cd "$1" && pwd)"; shift
R=/root/jiwon/Workspace/p4c
E2E=$R/tools/p4c-e2e-scripts
LIST=/root/jiwon/Workspace-remote/usenix27/program_list_151_no_oom.txt
created=()
for n in $(seq 0 7); do                      # tampering.py: port n <-> veth{2n}/veth{2n+1}
  if [ ! -e /sys/class/net/veth$((2 * n)) ]; then
    ip link add veth$((2 * n)) type veth peer name veth$((2 * n + 1))
    created+=($((2 * n)))
  fi
  ip link set veth$((2 * n)) up; ip link set veth$((2 * n + 1)) up
done
trap 'for v in "${created[@]}"; do ip link del veth$v 2>/dev/null; done' EXIT
for pdir in "$CAMP"/*/; do
  P=$(basename "$pdir")
  [ $# -gt 0 ] && [[ " $* " != *" $P "* ]] && continue
  read -r _ p4 tgt _ < <(grep -E "^$P " "$LIST" | sed 's#~/#/root/#g')
  [ "${tgt:-}" = bmv2 ] || continue
  for cdir in "$pdir"*/; do
    cat=$(basename "$cdir")
    ls "$cdir"*.txtpb >/dev/null 2>&1 || continue
    out="$CAMP/_replay_bmv2/$cat"; mkdir -p "$out/$P/protobuf" "$out/$P/build"
    cp "$cdir"*.txtpb "$out/$P/protobuf/"
    "$R/build/backends/bmv2/p4c-bm2-ss" -I "$R/p4include" -o "$out/$P/build/$P.json" \
        --p4runtime-files "$out/$P/build/${P}_p4info.txt,$out/$P/build/${P}_p4info.json" "$p4" \
        || { echo "$P $cat: compile failed"; continue; }
    python3 "$E2E/cp_snapshot/snapshot_ids.py" "$out/$P/build/${P}_p4info.json" "$out/$P/protobuf/"*.txtpb
    docker run --rm --net=host --privileged -v /root/jiwon:/root/jiwon -w "$E2E" \
        p4lang/behavioral-model:latest bash -c '
          printf "#!/bin/sh\nexec \"\$@\"\n" > /usr/local/bin/sudo && chmod +x /usr/local/bin/sudo
          python3 tampering.py --target bmv2 --stage run --p4-file "$0" --target-name "$1" \
              --output-root "$2" --csv "$2/results.csv" --tamper-mode both --no-ui' \
        "$p4" "$P" "$out" > "$out/$P.log" 2>&1
    echo "$P $cat rc=$? $(grep -c '"OK"' "$out/results.csv" 2>/dev/null) OK / $(ls "$cdir"*.txtpb | wc -l) cases"
  done
done
