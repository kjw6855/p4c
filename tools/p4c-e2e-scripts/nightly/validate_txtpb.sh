#!/usr/bin/env bash
# Validate (replay) the txtpb test cases that nightly_sweep.sh generated under <current>
# (or <previous> with --prev). Runs `tampering.py --stage run` per matrix cell; no
# generation, no rotation, no email.
# Meant for MANUAL use (needs sudo + a live dataplane), unlike the cron-only nightly_sweep.sh.
#
# Scope it to one cell / program with the selectors below; with none, it walks all six cells
# that have generated output. Results land in each cell's tampering_results.csv (written by
# tampering.py); a per-run log is written to <run-dir>/validate.log.
#
#   ./validate_txtpb.sh [--prev] [--target bmv2|tofino] [--arch v1model|tna] [--tamper key|cond] [--filter PAT]
#
# Example: validate just the tofino/tna condition test for one program (in the current run)
#   ./validate_txtpb.sh --target tofino --arch tna --tamper cond --filter heavy_hitter_5tupple
# Example: re-validate everything from the previous run
#   ./validate_txtpb.sh --prev
set -uo pipefail

SCRIPTDIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
P4CSD="${SWEEP_P4CSD:-$(dirname "$SCRIPTDIR")}"
# Corpus checkout holding the .p4 sources and the *_program_list.txt files. Overridable
# because the whole tree can live under a different root (e.g. a shared box where each user
# has their own ~/<user>/Workspace-remote); OUTROOT and ROOT already were.
TR="${SWEEP_TOP_TIER_REPO:-/home/vagrant/Workspace-remote/top_tier_repo}"
ROOT="${SWEEP_ROOT:-/home/vagrant/p4symbex_nightly}"      # local control dir: shared lock
OUTROOT="${SWEEP_OUTROOT:-/home/vagrant/Workspace-remote/p4symbex_nightly}"
JOBS="${SWEEP_JOBS:-3}"
export PATH="/home/vagrant/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

# ---- CLI parsing -----------------------------------------------------------------------------
OPT_TARGET=""; OPT_ARCH=""; OPT_TAMPER=""; CLI_FILTER=""; RUNDIR=current
USAGE="usage: $(basename "$0") [--prev] [--target bmv2|tofino] [--arch v1model|tna] [--tamper key|cond] [--filter PAT]"
while [ $# -gt 0 ]; do
  case "$1" in
    --prev)     RUNDIR=previous ;;
    --target)   shift; OPT_TARGET="${1:-}" ;;
    --target=*) OPT_TARGET="${1#*=}" ;;
    --arch)     shift; OPT_ARCH="${1:-}" ;;
    --arch=*)   OPT_ARCH="${1#*=}" ;;
    --tamper)   shift; OPT_TAMPER="${1:-}" ;;
    --tamper=*) OPT_TAMPER="${1#*=}" ;;
    --filter)   shift; CLI_FILTER="${1:-}" ;;
    --filter=*) CLI_FILTER="${1#*=}" ;;
    -h|--help)  echo "$USAGE"; exit 0 ;;
    *)          echo "unknown argument: $1" >&2; echo "$USAGE" >&2; exit 2 ;;
  esac
  shift
done

# Validate cell selectors and map --tamper to its chain-list token (key->h2s2k, cond->h2s2c).
case "$OPT_TARGET" in ""|bmv2|tofino) ;; *) echo "invalid --target: $OPT_TARGET (bmv2|tofino)" >&2; exit 2 ;; esac
case "$OPT_ARCH"   in ""|v1model|tna) ;; *) echo "invalid --arch: $OPT_ARCH (v1model|tna)" >&2; exit 2 ;; esac
TAMPER_LIST=""
case "$OPT_TAMPER" in
  "")   ;;
  key)  TAMPER_LIST=h2s2k ;;
  cond) TAMPER_LIST=h2s2c ;;
  *)    echo "invalid --tamper: $OPT_TAMPER (key|cond)" >&2; exit 2 ;;
esac
FILTER="$CLI_FILTER"
FILTER_ARG=(); [ -n "$FILTER" ] && FILTER_ARG=(--filter "$FILTER")

# Same (target arch list) matrix as nightly_sweep.sh.
MATRIX=(
  "bmv2 v1model h2s2k"   "bmv2 v1model h2s2c"
  "tofino v1model h2s2k" "tofino v1model h2s2c"
  "tofino tna h2s2k"     "tofino tna h2s2c"
)
# Narrow the matrix by any subset of --target/--arch/--tamper (unset selector = match all).
if [ -n "$OPT_TARGET$OPT_ARCH$TAMPER_LIST" ]; then
  SELECTED=()
  for entry in "${MATRIX[@]}"; do
    read -r tgt arch list <<<"$entry"
    [ -n "$OPT_TARGET" ]  && [ "$tgt"  != "$OPT_TARGET" ]  && continue
    [ -n "$OPT_ARCH" ]    && [ "$arch" != "$OPT_ARCH" ]    && continue
    [ -n "$TAMPER_LIST" ] && [ "$list" != "$TAMPER_LIST" ] && continue
    SELECTED+=("$entry")
  done
  if [ ${#SELECTED[@]} -eq 0 ]; then
    echo "no matrix cells match --target='$OPT_TARGET' --arch='$OPT_ARCH' --tamper='$OPT_TAMPER'" >&2
    exit 2
  fi
  MATRIX=("${SELECTED[@]}")
fi

# Share nightly_sweep.sh's lock so validation never runs while a gen sweep rotates <current>.
mkdir -p "$ROOT"
exec 9>"$ROOT/.lock"
if ! flock -n 9; then
  echo "a sweep/validation is already running (lock $ROOT/.lock held)" >&2; exit 1
fi

RUN="$OUTROOT/$RUNDIR"
if [ ! -d "$RUN" ]; then
  echo "no $RUNDIR run dir at $RUN — run nightly_sweep.sh (gen) first" >&2; exit 1
fi
LOG="$RUN/validate.log"
echo "=== validate txtpb $(date) ${FILTER:+(filter=$FILTER)} ===" >"$LOG"

for entry in "${MATRIX[@]}"; do
  read -r tgt arch list <<<"$entry"
  out="$RUN/${tgt}_${arch}_${list}"
  echo "--- $tgt/$arch $list ---" >>"$LOG"
  if [ ! -d "$out" ]; then
    echo "  (no generated output dir; skipping)" >>"$LOG"; continue
  fi
  # Tofino replay needs the SDE managed (build + start/stop tofino_model & switchd) and a
  # full restart between tests for a clean register state.
  SDE_ARG=()
  [ "$tgt" = tofino ] && SDE_ARG=(--sde-manage-procs --sde-cmake-build --reset-mode restart)
# Per-cell wall clock. A cell must fit `2 x max_tier x ceil(n_expensive / JOBS)` plus the cheap tail,
# because --tamper-mode both runs the two policies SEQUENTIALLY and gives each the FULL per-program
# budget. At today's tiers a 41-program h2s2k cell needs ~29h and a 64-program h2s2c cell ~40h at
# JOBS=4, so the 6h default cannot cover a full cell whose heavy programs run to completion -- it
# silently truncates the run instead (exit=124, later programs never attempted). Raise it for a
# catch-up sweep; the default keeps the nightly cron exactly as it was.
  timeout "${SWEEP_CELL_TIMEOUT:-6h}" python3 "$P4CSD/tampering.py" \
      --target "$tgt" --arch "$arch" --stage run --tamper-mode both \
      --program-list "$TR/${list}_program_list.txt" --output-root "$out" \
      --jobs "$JOBS" --no-ui "${SDE_ARG[@]}" "${FILTER_ARG[@]}" \
      >>"$LOG" 2>&1
  echo "  exit=$? at $(date)" >>"$LOG"
done

echo "=== validate done $(date) ===" >>"$LOG"
echo "validation complete; results under $RUN (CSV per cell, log at $LOG)"
