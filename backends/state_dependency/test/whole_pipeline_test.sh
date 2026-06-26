#!/usr/bin/env bash
# Regression test for whole-pipeline state-dependency (Parser->Ingress cross-block provenance).
#
# Asserts that with --whole-pipeline the parser-sourced header (hdr.ipv4.identification) appears in the
# ingress SO->condition chain's dependency graph, while per-control analysis (no flag) does NOT see it
# (the chain is rooted at unsourced metadata). See whole_pipeline_v1model.p4 for the program.
#
# Override the binary with P4SD_BIN=/path/to/p4c_state_dependency.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${P4SD_BIN:-$ROOT/build/p4c_state_dependency}"
P4="$HERE/whole_pipeline_v1model.p4"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/pc" "$tmp/wp"

"$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL \
    --graphs-dir "$tmp/pc" "$P4" >/dev/null 2>&1
"$BIN" --arch v1model --std p4-16 -I "$ROOT/p4include" --supergraph FULL --whole-pipeline \
    --graphs-dir "$tmp/wp" "$P4" >/dev/null 2>&1

pc=$(grep -c identification "$tmp/pc/MyIngress_h2s2c_dep.dot" 2>/dev/null || true)
wp=$(grep -c identification "$tmp/wp/v1_pipe_h2s2c_dep.dot" 2>/dev/null || true)
pc=${pc:-0}; wp=${wp:-0}
echo "per-control header-in-chain=$pc  whole-pipeline header-in-chain=$wp"

if [ "$pc" -eq 0 ] && [ "$wp" -ge 1 ]; then
    echo "PASS: whole-pipeline recovers parser->ingress header provenance"
    exit 0
fi
echo "FAIL: expected per-control=0 and whole-pipeline>=1"
exit 1
