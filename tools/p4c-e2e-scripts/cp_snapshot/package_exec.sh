#!/usr/bin/env bash
# package_exec.sh <program> [options]: what every programs/<target>/<arch>/<program>/exec.sh runs.
# Captures the program's live control-plane snapshot (capture.py, scenario.py of the package) and
# checks that p4symbex loads it; optionally runs p4symbex on it (--symbex) or replays the package's
# exploits with the program's controller in the loop (--validate, validate_live.py).
set -euo pipefail
E2E=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
P=${1:?usage: package_exec.sh <program> [options]}; shift
PKG=$(ls -d "$E2E"/cp_snapshot/programs/*/*/"$P" 2>/dev/null | head -1)
[ -n "$PKG" ] || { echo "no package for $P under $E2E/cp_snapshot/programs" >&2; exit 2; }
WORK=${WORK:-/root/jiwon/Workspace-remote/sp27/cp_live/$P}
OUT=${OUT:-$WORK/snapshot/$P.txtpb}
LOCK=/root/jiwon/Workspace-remote/sp27/cp_live/.capture.lock
SYMBEX=0 VALIDATE=0 CAPTURE=1 CASES=$PKG/exploits/SEED SEQ= OUT_GIVEN=0
VOUT=${VOUT:-$PKG/validate}
DESC=$(cd "$E2E" && python3 -c "
import sys; sys.path.insert(0, '.')
from cp_snapshot import packages
m = packages.load_module(packages.scenario_path(sys.argv[1]), 'live')
print(getattr(m, 'TARGET', 'tofino') + ': ' + getattr(m, 'DESCRIPTION', ''))" "$P")

usage() {
  cat <<USAGE
Usage: ./exec.sh [--output FILE] [--sequence SCENARIO] [--symbex] [--validate [CASES_DIR]] [--help]

$P ($DESC)

Default: capture the live control-plane snapshot and check that p4symbex loads it.
Options:
  -o, --output FILE        where capture writes the snapshot (default: $(realpath -m "$OUT"))
      --sequence SCENARIO  capture init plus one diff per controller step of SCENARIO (scenario.py
                           STEPS/SCENARIOS) as a CpSnapshotSequence (default output:
                           $WORK/snapshot/seq/SCENARIO.txtpb)
      --symbex             after the capture, run p4symbex H2S2 {OUTK,OUTC,O} into $WORK/symbex
      --validate [DIR]     no capture: replay DIR/<category>/*.txtpb (default exploits/SEED) with the
                           program's controller in the loop; verdicts into $VOUT
  -h, --help               show this help

Package (committed): $PKG
  snapshot/live.txtpb, snapshot/static.txtpb   captured and hand-written snapshots
  exploits/{SEED,INITIAL}/<category>/          p4symbex cases (--cp-snapshot-registers)
  validate/                                    snapshot_replay.csv, controller_replay.csv, drift.txt
Work dir (patched controller copy, logs, pcaps; not committed): $WORK

Env: WORK, OUT; --sequence: SETTLE seconds after each step (1.0), CHECK_DETERMINISM=1 runs the
setup twice and fails if the dumps differ; --symbex: REGS=SEED|INITIAL (SEED), TIMEOUT (1800), JOBS (3), CATS ("OUTK OUTC O");
--validate: SETTLE seconds after each phase (1.0), VOUT verdict directory (the package's validate/).
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -o|--output) [ $# -ge 2 ] || { echo "$1 needs a file" >&2; exit 2; }; OUT=$2; OUT_GIVEN=1; shift 2 ;;
    --output=*) OUT=${1#--output=}; OUT_GIVEN=1; shift ;;
    --symbex) SYMBEX=1; shift ;;
    --sequence) [ $# -ge 2 ] || { echo "$1 needs a scenario" >&2; exit 2; }; SEQ=$2; shift 2 ;;
    --validate) VALIDATE=1; CAPTURE=0; shift
                if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then CASES=$1; shift; fi ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
OUT=$(realpath -m "$OUT")
# A sequence goes to its own file unless --output names one.
[ -n "$SEQ" ] && [ "$OUT_GIVEN" = 0 ] && OUT=$WORK/snapshot/seq/$SEQ.txtpb
mkdir -p "$WORK"

# Captures and validations share tofino-model/bmv2 ports and veths: one at a time.
exec 9>"$LOCK"
flock -n 9 || { echo "another capture/validation holds $LOCK; waiting"; flock 9; }

if [ "$CAPTURE" = 1 ]; then
  if [ -n "$SEQ" ]; then
    echo "== capture sequence $SEQ -> $OUT"
    HOME=/root/jiwon python3 "$E2E/cp_snapshot/capture.py" "$P" --work "$WORK" --out "$OUT" \
        --sequence "$SEQ" --settle "${SETTLE:-1.0}" ${CHECK_DETERMINISM:+--check-determinism}
    echo "sequence: $OUT"
  else
    echo "== capture -> $OUT"
    HOME=/root/jiwon python3 "$E2E/cp_snapshot/capture.py" "$P" --work "$WORK" --out "$OUT"
    echo "snapshot: $OUT"
    echo "== p4symbex load check"
    bash "$E2E/cp_snapshot/check_snapshot.sh" "$P" "$OUT"
  fi
fi

if [ "$VALIDATE" = 1 ]; then
  CASES=$(realpath "$CASES")
  echo "== controller-in-the-loop validation of $CASES -> $VOUT"
  base=()
  [ -f "$VOUT/snapshot_replay.csv" ] && base=(--baseline "$VOUT/snapshot_replay.csv")
  ref=()
  [ -f "$WORK/entities.pkl" ] && ref=(--reference "$WORK/entities.pkl")
  HOME=/root/jiwon python3 "$E2E/cp_snapshot/validate_live.py" "$P" "$CASES" \
      --work "$WORK/_validate" --out "$VOUT" --settle "${SETTLE:-1.0}" "${base[@]}" "${ref[@]}"
fi
flock -u 9

if [ "$SYMBEX" = 1 ]; then
  echo "== p4symbex H2S2 -> $WORK/symbex"
  # campaign.sh reads <SNAPS>/<program>.txtpb, so link the snapshot in under that name.
  mkdir -p "$WORK/symbex/_snap"
  ln -sfn "$OUT" "$WORK/symbex/_snap/$P.txtpb"
  SNAPS=$WORK/symbex/_snap REGS=${REGS:-SEED} JOBS=${JOBS:-3} \
    bash "$E2E/cp_snapshot/campaign.sh" "$WORK/symbex" "$P"
fi
