#!/usr/bin/env bash
# Nightly p4symbex tampering sweep (GENERATION only) -> report -> email (msmtp).
# Invoked by cron at 03:30 local. Self-contained; no Claude required.
#
# This script is the cron path: for every matrix cell it runs `tampering.py --stage gen`
# to generate txtpb test cases (h2s2k + h2s2c), rotates current/previous, builds the
# report, and emails it. It does NOT replay/validate — use validate_txtpb.sh for that.
#
# Lives in the p4c-e2e-scripts repo (nightly/); each VM exposes it at
# /home/vagrant/p4symbex_nightly/nightly_sweep.sh via a symlink, so SCRIPTDIR below
# resolves the symlink to find build_report.py next to the real script.
#
# Guard: skips if a P4/symbex BUILD is in progress (cc1plus/make/p4c/bf-p4c) so the cron
# run never collides with you compiling.
#
# Storage: run output (current/previous) lives under $OUTROOT (Workspace-remote); only the
# lock and skip log stay local under $ROOT. Keeps only CURRENT and PREVIOUS (older deleted).
set -uo pipefail

# Resolve through the symlink so build_report.py (shipped beside this script) is found.
SCRIPTDIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# tampering.py is bundled at the repo root (one level up from nightly/); override with $SWEEP_P4CSD.
P4CSD="${SWEEP_P4CSD:-$(dirname "$SCRIPTDIR")}"
# Corpus checkout holding the .p4 sources and the *_program_list.txt files. Overridable
# because the whole tree can live under a different root (e.g. a shared box where each user
# has their own ~/<user>/Workspace-remote); OUTROOT and ROOT already were.
TR="${SWEEP_TOP_TIER_REPO:-/home/vagrant/Workspace-remote/top_tier_repo}"
ROOT="${SWEEP_ROOT:-/home/vagrant/p4symbex_nightly}"      # local control dir: lock + skip log
OUTROOT="${SWEEP_OUTROOT:-/home/vagrant/Workspace-remote/p4symbex_nightly}"  # run output: current/previous
MAILTO="${SWEEP_MAILTO:-}"   # set in the per-VM cron line; kept out of the repo
JOBS="${SWEEP_JOBS:-4}"  # safe on 64GB: cache-build IFDS analyses peak ~10-14GB each (OOM'd at 3 on 32GB)
MAXTESTS="${SWEEP_MAXTESTS:-4}"  # bound nightly runtime; raise for deeper sweeps
# Cache mode. Default: --parser-deps (seed parser-derived metadata as sources; lighter/faster than
# whole-pipeline while still capturing cross-block chains) under the baseline cache name.
# Whole-pipeline mode (opt-in via SWEEP_WHOLE_PIPELINE): build/load a cache of cross-block
# (Parser->Ingress/Egress) chains under a SEPARATE cache name so it never clobbers the baseline cache.
# p4symbex loads either via the cache (no p4symbex flag needed — the cache holds the chains).
if [ -n "${SWEEP_WHOLE_PIPELINE:-}" ]; then
  CACHE_NAME="${SWEEP_CACHE_NAME:-sochain_cache_wp}"
  CACHE_MODE_ARG=(--whole-pipeline)
else
  CACHE_NAME="${SWEEP_CACHE_NAME:-sochain_cache}"
  CACHE_MODE_ARG=(--parser-deps)
fi
# SOChain cache regeneration is OPT-IN — the cache is a global, reused artifact, so a plain run loads
# it as-is and does NOT re-derive it. Two opt-in modes, handled in the cache section below:
#   SWEEP_CACHE_REFRESH=1 -> incremental refresh: cache_chains.py rebuilds only the curated
#                            h2s2k/h2s2c entries whose analyzer binary or .p4 source is newer.
#   SWEEP_CACHE_FORCE=1   -> FULL re-derivation: force-analyze the entire program_list.txt (216),
#                            recategorize the h2s2k/h2s2c lists in place, then gen over the fresh lists.
# Default (neither set): use the existing $CACHE_DIR as-is; bootstrap-build it only if absent.
CACHE_DIR="$OUTROOT/$CACHE_NAME"  # GLOBAL SOChain cache: reused across runs, NOT under current/previous
# Cache-builder binary, used ONLY for the snapshot-before-refresh staleness check below. cache_chains.py
# now defaults to --builder p4symbex, so the reference binary is p4symbex (a rebuild of it invalidates
# the cache). Override with SWEEP_SD_BIN when running --builder analyzer.
SD_BIN="${SWEEP_SD_BIN:-$HOME/Workspace/p4c/build/p4symbex}"  # cache builder (for staleness check)
export PATH="/home/vagrant/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

# One-deep snapshot of the cache before it is regenerated, so the prior chains are preserved at
# ${CACHE_DIR}_old (a complete, loadable cache you can roll back to / diff against).
snapshot_cache() {
  [ -d "$CACHE_DIR" ] || return 0
  rm -rf "${CACHE_DIR}_old"
  cp -a "$CACHE_DIR" "${CACHE_DIR}_old" \
    && echo "  snapshot: $CACHE_DIR -> ${CACHE_DIR}_old ($(date))" >>"${LOG:-/dev/null}"
}

# (target arch list) matrix — pna excluded by omission.
MATRIX=(
  "bmv2 v1model h2s2k"   "bmv2 v1model h2s2c"
  "tofino v1model h2s2k" "tofino v1model h2s2c"
  "tofino tna h2s2k"     "tofino tna h2s2c"
)
# Testing overrides: SWEEP_MATRIX="tofino tna h2s2k" (one cell), SWEEP_FILTER=switchv2p (one program).
#
# Several cells may be given, separated by ';' or newlines — a whole policy is three cells
# ("bmv2 v1model h2s2k;tofino v1model h2s2k;tofino tna h2s2k"), which is how a sweep is split across
# machines by policy. They have to be ONE run: each run rotates current/ into previous/, so invoking
# the script once per cell would leave only the last two cells' output on disk.
if [ -n "${SWEEP_MATRIX:-}" ]; then
  MATRIX=()
  while IFS= read -r cell; do
    cell="${cell#"${cell%%[![:space:]]*}"}"   # ltrim
    cell="${cell%"${cell##*[![:space:]]}"}"   # rtrim
    [ -n "$cell" ] && MATRIX+=("$cell")
  done < <(printf '%s\n' "$SWEEP_MATRIX" | tr ';' '\n')
fi
FILTER_ARG=(); [ -n "${SWEEP_FILTER:-}" ] && FILTER_ARG=(--filter "$SWEEP_FILTER")

# External control-plane / port annotations. Without this the sweep generates as if no annotation
# existed: no clause pruning, no port verdicts, no register initial values. Default ON, pointing at
# the annotations dir beside the cache; set SWEEP_CP_ANNOTATIONS=none to reproduce old behaviour.
CP_ANN_DIR="${SWEEP_CP_ANNOTATIONS:-$OUTROOT/annotations}"
CP_ANN_ARG=()
if [ "$CP_ANN_DIR" != "none" ] && [ -d "$CP_ANN_DIR" ]; then
  CP_ANN_ARG=(--cp-annotation-root "$CP_ANN_DIR")
fi

# Search shape. Neither changes which tests are correct; both matter under the per-program timeout,
# where chain order decides what a truncated run reached. Override with SWEEP_TRAVERSAL=PHASE1 (or
# NONE for the measurement baseline) and SWEEP_IMPACT_ORDER=0.
TRAVERSAL_ARG=(); [ -n "${SWEEP_TRAVERSAL:-PHASE1_PHASE2}" ] && \
  TRAVERSAL_ARG=(--shared-traversal "${SWEEP_TRAVERSAL:-PHASE1_PHASE2}")
IMPACT_ARG=(); [ "${SWEEP_IMPACT_ORDER:-1}" = "1" ] && IMPACT_ARG=(--chain-impact-order)

mkdir -p "$ROOT" "$OUTROOT"
exec 9>"$ROOT/.lock"
if ! flock -n 9; then echo "$(date): skipped — a sweep is already running" >>"$ROOT/skip.log"; exit 0; fi

build_active() {
  for p in cc1plus cc1 make p4c bf-p4c p4c-barefoot; do
    pgrep -x "$p" >/dev/null 2>&1 && return 0
  done
  return 1
}
if build_active; then
  echo "$(date): skipped — build in progress" >>"$ROOT/skip.log"; exit 0
fi

# Rotate: keep only previous + current.
rm -rf "$OUTROOT/previous"
[ -d "$OUTROOT/current" ] && mv "$OUTROOT/current" "$OUTROOT/previous"
RUN="$OUTROOT/current"; mkdir -p "$RUN"
LOG="$RUN/sweep.log"
echo "=== nightly sweep $(date) ===" >"$LOG"

# GLOBAL SOChain cache (shared by every matrix cell and tamper-mode policy; lives outside
# current/previous). p4symbex loads chains via --state-dep-cache instead of re-running the
# ~minutes-long IFDS analysis per program per policy. Regeneration is OPT-IN (see the SWEEP_CACHE_*
# notes above): a plain run loads the cache as-is; only SWEEP_CACHE_REFRESH / SWEEP_CACHE_FORCE
# re-derive. Non-fatal: a program whose cache is missing/stale hard-errors under
# --state-dep-cache-root below, surfacing in the report.
if [ -n "${SWEEP_CACHE_FORCE:-}" ]; then
  # FULL re-derivation: (1) force-analyze all 216 programs into the cache, (2) recategorize the
  # h2s2k/h2s2c lists in place from that cache, so (3) the gen loop below runs over freshly-derived
  # lists. SWEEP_FILTER is intentionally NOT applied here — recategorizing from a partial cache would
  # drop programs from the lists.
  snapshot_cache   # FORCE always regenerates -> back up first
  echo "--- [FULL] force-caching program_list.txt (216) under $CACHE_DIR ($(date)) ---" >>"$LOG"
  timeout "${SWEEP_CACHE_TIMEOUT:-10h}" python3 "$SCRIPTDIR/cache_chains.py" \
      --outroot "$OUTROOT" --cache-name "$CACHE_NAME" \
      --program-list "$TR/program_list.txt" \
      --jobs "$JOBS" "${CACHE_MODE_ARG[@]}" --force >>"$LOG" 2>&1
  echo "  [FULL] cache build exit=$? at $(date)" >>"$LOG"
  echo "--- [FULL] recategorizing h2s2k/h2s2c lists from cache ($(date)) ---" >>"$LOG"
  python3 "$SCRIPTDIR/categorize_chains.py" \
      --cache "$CACHE_DIR" --program-list "$TR/program_list.txt" \
      --current-h2s2k "$TR/h2s2k_program_list.txt" \
      --current-h2s2c "$TR/h2s2c_program_list.txt" \
      --write-dir "$TR" >>"$LOG" 2>&1
  echo "  [FULL] categorize exit=$? at $(date)" >>"$LOG"
elif [ -n "${SWEEP_CACHE_REFRESH:-}" ]; then
  # Incremental refresh (opt-in): rebuild only the curated entries that are stale.
  # Back up only if the mtime check will actually regenerate something: any cached .chains not newer
  # than the analyzer binary is stale and about to be rebuilt.
  if [ -d "$CACHE_DIR" ] && [ -e "$SD_BIN" ] \
     && [ -n "$(find -L "$CACHE_DIR" -name '*.chains' ! -newer "$SD_BIN" -print -quit 2>/dev/null)" ]; then
    snapshot_cache
  fi
  echo "--- refreshing SOChain cache under $CACHE_DIR ($(date)) ---" >>"$LOG"
  timeout 4h python3 "$SCRIPTDIR/cache_chains.py" \
      --outroot "$OUTROOT" --cache-name "$CACHE_NAME" --program-lists "$TR" \
      --jobs "$JOBS" "${CACHE_MODE_ARG[@]}" "${FILTER_ARG[@]}" >>"$LOG" 2>&1
  echo "  cache refresh exit=$? at $(date)" >>"$LOG"
elif [ -d "$CACHE_DIR" ] && [ -n "$(find -L "$CACHE_DIR" -name '*.chains' -print -quit 2>/dev/null)" ]; then
  # DEFAULT: regeneration is opt-in; a populated cache is loaded as-is (no re-derivation).
  echo "--- using existing SOChain cache at $CACHE_DIR as-is; regeneration is opt-in" \
       "(SWEEP_CACHE_REFRESH=1 to refresh stale entries, SWEEP_CACHE_FORCE=1 to full-rederive) ($(date)) ---" >>"$LOG"
else
  # No cache present: bootstrap once (gen hard-errors under --state-dep-cache-root without it).
  echo "--- WARNING: no SOChain cache at $CACHE_DIR; bootstrap-building it once" \
       "(regeneration is otherwise opt-in) ($(date)) ---" >>"$LOG"
  timeout 4h python3 "$SCRIPTDIR/cache_chains.py" \
      --outroot "$OUTROOT" --cache-name "$CACHE_NAME" --program-lists "$TR" \
      --jobs "$JOBS" "${CACHE_MODE_ARG[@]}" "${FILTER_ARG[@]}" >>"$LOG" 2>&1
  echo "  cache bootstrap exit=$? at $(date)" >>"$LOG"
fi

# Log the expected per-program p4symbex timeouts (from the symbex_timeout= tags in the lists the gen
# step is about to use), grouped one line per distinct timeout value in increasing order. Gives an
# up-front picture of the scheduler's budget (which programs are the expensive ones).
echo "--- expected p4symbex per-program timeouts ($(date)) ---" >>"$LOG"
python3 - "$TR/h2s2k_program_list.txt" "$TR/h2s2c_program_list.txt" >>"$LOG" 2>&1 <<'PY'
import sys
from collections import defaultdict
groups = defaultdict(set)  # timeout -> {program names}
for path in sys.argv[1:]:
    try:
        lines = open(path).read().splitlines()
    except OSError:
        continue
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        toks = ln.split()
        name = toks[0]
        t = 1800
        for tok in toks[1:]:
            if tok.startswith("symbex_timeout="):
                try:
                    t = int(tok.split("=", 1)[1])
                except ValueError:
                    pass
        groups[t].add(name)
for t in sorted(groups):
    progs = " ".join(sorted(groups[t]))
    print(f"[timeouts] {t}s ({len(groups[t])}): {progs}")
PY

for entry in "${MATRIX[@]}"; do
  read -r tgt arch list <<<"$entry"
  pl="$TR/${list}_program_list.txt"
  out="$RUN/${tgt}_${arch}_${list}"
  echo "--- $tgt/$arch $list ($pl) ---" >>"$LOG"
  [ -f "$pl" ] || { echo "  (missing program list)" >>"$LOG"; continue; }
# Per-cell wall clock. A cell must fit `2 x max_tier x ceil(n_expensive / JOBS)` plus the cheap tail,
# because --tamper-mode both runs the two policies SEQUENTIALLY and gives each the FULL per-program
# budget. At today's tiers a 41-program h2s2k cell needs ~29h and a 64-program h2s2c cell ~40h at
# JOBS=4, so the 6h default cannot cover a full cell whose heavy programs run to completion -- it
# silently truncates the run instead (exit=124, later programs never attempted). Raise it for a
# catch-up sweep; the default keeps the nightly cron exactly as it was.
  timeout "${SWEEP_CELL_TIMEOUT:-6h}" python3 "$P4CSD/tampering.py" \
      --target "$tgt" --arch "$arch" --stage gen --tamper-mode both \
      --program-list "$pl" --output-root "$out" --jobs "$JOBS" --max-tests "$MAXTESTS" \
      --state-dep-cache-root "$CACHE_DIR" \
      "${CP_ANN_ARG[@]}" "${TRAVERSAL_ARG[@]}" "${IMPACT_ARG[@]}" \
      "${FILTER_ARG[@]}" \
      >>"$LOG" 2>&1
  echo "  exit=$? at $(date)" >>"$LOG"
done

# Report + email.
REPORT="$RUN/report.eml"
python3 "$SCRIPTDIR/build_report.py" "$RUN" >"$REPORT" 2>>"$LOG"
if [ -z "$MAILTO" ]; then
  echo "$(date): SWEEP_MAILTO unset — report left at $REPORT, not emailed" >>"$LOG"
elif msmtp "$MAILTO" <"$REPORT" 2>>"$LOG"; then
  echo "$(date): report emailed to $MAILTO" >>"$LOG"
else
  echo "$(date): EMAIL FAILED (see ~/.msmtp.log)" >>"$LOG"
fi
echo "=== done $(date) ===" >>"$LOG"
