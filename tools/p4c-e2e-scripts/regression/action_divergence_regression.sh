#!/usr/bin/env bash
#
# action_divergence_regression.sh — generation-level regression for the sink action-divergence
# gate (core/symbolic_executor/state_dependency_track.cpp::sinkActionsDiverge).
#
# Runs p4symbex tampering generation on two real H2S2K programs, each checked by its own mode:
#   - SwitchV2P (mode=count, full recipe): terminates quickly. Assert exit 0 and that the emitted
#     *.txtpb count matches a recorded baseline.
#   - fisslock  (mode=count, full recipe): asserts exit 0, the emitted count, AND that cases whose
#     `affected_register` is `lock_rw_mode_array` survive (see "why content" below).
#
# Why SwitchV2P moved off the bare recipe (2026-08-13): its sink is gated by `switch_config`, a
# keyless table whose `set_config(switch_type, switch_id)` the controller installs once per device.
# With the cross-phase control-plane pin, a candidate needing Phase 1 and Phase 2 to run under
# different switch roles is unrealizable and correctly dropped — so WITHOUT the annotation that
# states which role is deployed, SwitchV2P emits 0. A baseline of 0 is worthless as a regression
# gate: it passes identically if generation breaks completely. The annotation
# (`assume: action_data(set_config, switch_type) == 0`, i.e. TOR, matching the shipped PTF harness)
# pins the role, and the arm asserts a real 8. This also matches what the nightly already does —
# nightly_sweep.sh passes --cp-annotation-root by default.
#
# Why content, not just a count: fisslock used to run as mode=smoke (non-crash only, count
# informational). That gate PASSED while the reverted P1b vacuous-write patch was destroying every
# `lock_rw_mode_array` case — i.e. the exact cases that replay VULNERABLE. A count alone is also too
# weak, because a patch can hold the total steady while swapping which register is tampered:
# fisslock emits two families, `lock_rw_mode_array` (pure-read victim side, the true positives) and
# `lock_free_mode_array` (self-setting victim, the RC-1 false positives). Only the first is load
# bearing, so it is asserted by name. Note a plain grep for the register is VACUOUS — every fisslock
# txtpb mentions both registers in its `traces:` lines. The discriminator is the `affected_register`
# block, which is what CONTENT_REGISTER matches against.
#
# Why fisslock can now be asserted at all: mode=smoke existed because fisslock's DFS did not
# terminate. With the full corpus recipe (--state-dep-cache + --cp-annotation +
# --shared-traversal PHASE1_PHASE2 + --chain-impact-order) it completes, so the count is meaningful.
# Omitting any of those flags reverts it to a timeout that looks like a result but is not.
#
# This is generation-only (no SDE / no replay), so it runs anywhere the p4symbex binary builds.
# It is NOT wired into CI (it depends on external program paths under ~/Workspace-remote).
#
# Capturing baselines (first run after a change): leave the BASELINE_* var unset to run that program
# in RECORD mode — the script prints the observed count and passes. Then re-run with it set.
#
# Measured on 2026-08-08 at 019199955 (matches the nightly's fisslock h2s2k run exactly:
# 40 generated, of which 24 are lock_rw_mode_array = the 24 that replay OK, and 16 are
# lock_free_mode_array = the 16 that replay FAIL under RC-1):
#   BASELINE_FISSLOCK=40   (fisslock content: 24/40)
#
# SwitchV2P re-recorded 2026-08-13 after the cross-phase index/control-plane work: 12 (bare, before)
# -> 8 (full recipe). The 8 are families 2 and 4, each 4 cases, every one with switch_type = 0 in
# both [P1] and [P2]; family 3 (GW_TOR) is correctly gone, since it needed Phase 2 to run as a
# different switch role than Phase 1.
#   BASELINE_SWITCHV2P=8
#
# Usage:
#   ./action_divergence_regression.sh                                   # record counts
#   BASELINE_SWITCHV2P=8 BASELINE_FISSLOCK=40 ./...sh                   # assert both
#   PER_PROG_TIMEOUT=600 ./...sh                                        # shorter default budget
#
# Runtime: switchv2p ~2min, fisslock ~35min (3600s budget). Expect ~40min total.
#
set -u

HOME_DIR="${HOME:-/home/vagrant}"
P4SYMBEX="${P4SYMBEX:-$HOME_DIR/Workspace/p4c/build/p4symbex}"
REPO="${TOP_TIER_REPO:-$HOME_DIR/Workspace-remote/top_tier_repo}"
MAX_TESTS="${MAX_TESTS:-4}"
TAMPER_VALUE="${TAMPER_VALUE:-0x1}"
# Default per-program wall-clock budget. A program may override it via its TIMEOUT field below, so
# a slow one is reported on its own rather than starving the next.
PER_PROG_TIMEOUT="${PER_PROG_TIMEOUT:-2400}"

# Roots for the full-recipe inputs. A program opts in by setting its STEM field (below); the cache
# is <CACHE_ROOT>/tofino/tna/<stem>.chains and the annotation <ANNOTATION_ROOT>/<stem>.json.
CACHE_ROOT="${CACHE_ROOT:-$HOME_DIR/Workspace-remote/p4symbex_nightly/sochain_cache}"
ANNOTATION_ROOT="${ANNOTATION_ROOT:-$HOME_DIR/Workspace-remote/p4symbex_nightly/annotations}"

INCLUDES=(
    "-I$HOME_DIR/Workspace/p4c/backends/tofino/bf-p4c/p4include"
    "-I$HOME_DIR/Workspace/p4c/p4include"
)

# Minimum txtpb a "smoke" program must emit to pass. Default 0: the smoke test is purely a
# non-crash check (a slow program that emits nothing within its budget is not a failure). Set >0
# only with a generous PER_PROG_TIMEOUT if you also want to assert generation made progress.
SMOKE_MIN="${SMOKE_MIN:-0}"

# name | p4-file | baseline-env-var | mode | stem | content-register | timeout
#   mode:
#     count  — program terminates; assert exit 0 and txtpb count == baseline (RECORD mode if unset).
#     smoke  — program's DFS does not terminate in any practical window; assert only that it runs
#              without crashing (exit 0 or a clean timeout) and emits >= SMOKE_MIN txtpb. A crash
#              signal (SIGSEGV/SIGABRT, compiler bug, non-zero non-124 exit) is a FAIL.
#   stem   — empty: bare invocation. Non-empty: opt into the full corpus recipe, resolving the
#            SOChain cache and cp-annotation from that stem (both must exist or the program FAILs).
#   content-register — empty: no content check. Non-empty: at least one emitted case must name this
#            register in its `affected_register` block, else FAIL. Matched as a suffix, so the bare
#            name works regardless of the `IngressPipe.` control prefix.
#   timeout — empty: use PER_PROG_TIMEOUT. fisslock has 20 key chains, which puts it in the
#            <=20-chains/3600s tier of the nightly's TIMEOUT_TIERS.
PROGRAMS=(
    "switchv2p|$REPO/SwitchV2P/p4-prototype/p4/switchv2p.p4|BASELINE_SWITCHV2P|count|SwitchV2P_p4-prototype_p4_switchv2p||"
    "fisslock|$REPO/fisslock/switch/p4/switch.p4|BASELINE_FISSLOCK|count|fisslock_switch_p4_switch|lock_rw_mode_array|3600"
)

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

fail=0
record_mode=0
printf '== sink action-divergence regression (max-tests=%s, tamper=%s) ==\n' "$MAX_TESTS" "$TAMPER_VALUE"

if [[ ! -x "$P4SYMBEX" ]]; then
    echo "FAIL: p4symbex binary not found/executable: $P4SYMBEX" >&2
    exit 2
fi

for entry in "${PROGRAMS[@]}"; do
    IFS='|' read -r name p4file baseline_var mode stem content_reg prog_timeout <<< "$entry"
    outdir="$WORKDIR/$name"
    mkdir -p "$outdir"
    log="$WORKDIR/$name.log"
    budget="${prog_timeout:-$PER_PROG_TIMEOUT}"

    if [[ ! -f "$p4file" ]]; then
        echo "FAIL [$name]: p4 source not found: $p4file"
        fail=1
        continue
    fi

    # Full corpus recipe, opted into per program via STEM. All four flags are load bearing: without
    # them a slow program times out and reports rc=124 / 0 txtpb, which reads like a result.
    recipe_args=()
    if [[ -n "$stem" ]]; then
        cache="$CACHE_ROOT/tofino/tna/$stem.chains"
        annotation="$ANNOTATION_ROOT/$stem.json"
        if [[ ! -f "$cache" ]]; then
            echo "FAIL  [$name]: SOChain cache not found: $cache"
            fail=1
            continue
        fi
        if [[ ! -f "$annotation" ]]; then
            echo "FAIL  [$name]: cp-annotation not found: $annotation"
            fail=1
            continue
        fi
        recipe_args=(
            --state-dep-cache "$cache"
            --cp-annotation "$annotation"
            --shared-traversal PHASE1_PHASE2
            --chain-impact-order
        )
    fi

    timeout "$budget" "$P4SYMBEX" \
        --target tofino --arch tna --std p4-16 -D__TARGET_TOFINO__=1 \
        "${INCLUDES[@]}" \
        --test-backend BFRT \
        --packet-size-range 14:9600 \
        --track-coverage STATEMENTS \
        --max-tests "$MAX_TESTS" \
        --max-port 32 \
        --out-dir "$outdir" \
        --state-dep \
        --path-selection STATE_DEP_TAMPERING \
        "${recipe_args[@]}" \
        "$p4file" \
        --state-tamper-value "$TAMPER_VALUE" \
        > "$log" 2>&1
    rc=$?

    count=$(find "$outdir" -name '*.txtpb' | wc -l | tr -d ' ')

    # Content check: how many emitted cases tamper CONTENT_REGISTER. Read from the
    # `affected_register { register_name: ... }` block only -- a plain file-wide grep would match
    # every case, because the register also appears in the `traces:` lines of unrelated cases.
    content_count=0
    if [[ -n "$content_reg" ]]; then
        while IFS= read -r tf; do
            reg=$(awk '/^affected_register/ {inblk=1; next}
                       inblk && /register_name:/ {print; exit}' "$tf")
            case "$reg" in
                *"$content_reg"*) content_count=$((content_count + 1)) ;;
            esac
        done < <(find "$outdir" -name '*.txtpb')
    fi

    if [[ "$mode" == "smoke" ]]; then
        # Non-crash smoke test: exit 0 or a clean timeout (124) is fine; anything else is a crash.
        if [[ $rc -ne 0 && $rc -ne 124 ]]; then
            echo "FAIL  [$name]: p4symbex crashed (exit $rc, see $log)"
            tail -5 "$log" | sed 's/^/    /'
            fail=1
        elif [[ "$SMOKE_MIN" -gt 0 && "$count" -lt "$SMOKE_MIN" ]]; then
            echo "FAIL  [$name]: emitted $count txtpb (< SMOKE_MIN=$SMOKE_MIN) — generation stalled"
            fail=1
        else
            kind=$([[ $rc -eq 124 ]] && echo "clean timeout @${budget}s" || echo "exit 0")
            echo "PASS  [$name]: no crash ($kind), emitted $count txtpb (smoke; count informational)"
        fi
        continue
    fi

    # mode == count: program must terminate normally and match the baseline.
    if [[ $rc -eq 124 ]]; then
        echo "FAIL  [$name]: exceeded ${budget}s — expected to terminate (see $log)"
        fail=1
        continue
    fi
    if [[ $rc -ne 0 ]]; then
        echo "FAIL  [$name]: p4symbex exited $rc (see $log)"
        tail -5 "$log" | sed 's/^/    /'
        fail=1
        continue
    fi

    # Content assertion runs regardless of whether the count baseline is recorded or asserted: it is
    # the check that survives a patch which holds the total steady while eliminating the load-bearing
    # family (what the reverted P1b patch did to fisslock's lock_rw_mode_array cases).
    if [[ -n "$content_reg" ]]; then
        if [[ "$content_count" -eq 0 ]]; then
            echo "FAIL  [$name]: no emitted case has affected_register '$content_reg'" \
                 "($count txtpb total) — the load-bearing family was eliminated (see $log)"
            fail=1
            continue
        fi
        echo "      [$name]: $content_count/$count cases tamper '$content_reg'"
    fi

    baseline="${!baseline_var:-}"
    if [[ -z "$baseline" ]]; then
        echo "RECORD [$name]: exit 0, emitted $count txtpb  (set $baseline_var=$count to assert)"
        record_mode=1
    elif [[ "$count" -eq "$baseline" ]]; then
        echo "PASS  [$name]: exit 0, emitted $count txtpb (== baseline)"
    else
        echo "FAIL  [$name]: emitted $count txtpb, expected baseline $baseline"
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo "== RESULT: FAIL =="
    exit 1
fi
if [[ $record_mode -ne 0 ]]; then
    echo "== RESULT: RECORDED (no baselines asserted) =="
    exit 0
fi
echo "== RESULT: PASS =="
exit 0
