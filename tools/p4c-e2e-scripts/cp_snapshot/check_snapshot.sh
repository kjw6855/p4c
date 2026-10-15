#!/usr/bin/env bash
# check_snapshot.sh <program> [snapshot.txtpb]
# Loads a --cp-snapshot in p4symbex (one tampering test, nightly chain cache) and prints the
# snapshot load / closed-world lines. p4symbex hard-errors on any table, key, action, parameter or
# register name the program does not have, so rc=0 means every name resolved.
set -uo pipefail
P="$1"
SNAP="${2:-/root/jiwon/Workspace-remote/sp27/cp_snapshots/$P.txtpb}"
R=/root/jiwon/Workspace/p4c
LIST=/root/jiwon/Workspace-remote/usenix27/program_list_151_no_oom.txt
CACHE=/root/jiwon/Workspace-remote/sp27/p4symbex_nightly/sochain_cache
read -r _ p4 tgt arch _ < <(grep -E "^$P " "$LIST" | sed 's#~/#/root/#g')
[ -n "${p4:-}" ] || { echo "$P: not in $LIST"; exit 2; }
if [ "$tgt" = tofino ]; then
  T=(--target tofino --arch "$arch" --std p4-16 -D__TARGET_TOFINO__=1
     -I$R/backends/tofino/bf-p4c/p4include -I$R/p4include --test-backend BFRT)
else
  T=(--target bmv2 --arch v1model --std p4-16 -I$R/p4include --test-backend protobuf)
fi
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
(cd "$out" && HOME=/root/jiwon prlimit --as=34359738368 timeout "${TIMEOUT:-600}" "$R/build/p4symbex" \
   "${T[@]}" --max-tests 1 --out-dir "$out" --state-dep --path-selection STATE_DEP_TAMPERING \
   --state-dep-cache "$CACHE/$tgt/$arch/$P.chains" --cp-snapshot "$SNAP" "$p4" \
   --state-tamper-value 0xdeadbeef > "$out/log" 2>&1)
rc=$?
grep -h "CP snapshot\|error\|matches no table" "$out/log" | grep -v "#[0-9]*:" | head -${LINES_MAX:-12}
echo "$P rc=$rc $(grep -h 'Symbex Summary' "$out/log" | tail -1)"
