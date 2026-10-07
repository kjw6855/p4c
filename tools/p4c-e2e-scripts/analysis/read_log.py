#!/usr/bin/env python3
import sys
import re
import csv
from pathlib import Path
import argparse
from collections import Counter

# TODO: Differentiate: [B#->I] (bug pattern) vs [->I] (dependency pattern)
pattern_to_key = {
    "[ACT->]": "A",
    "->I]": "A->I",
    "->D]": "A->D",
    "[SO->]": "SO",
    "[->KEY]": "SO->KEY",
    "[->HDR/PORT]": "SO->HDR/PORT",
    "[HDR->]": "H->SO",
}

graph_metric_to_key = {
    "Total nodes:": "node",
    "Total edges:": "edge",
    "Total ESG nodes:": "esgNode",
    "Total ESG edges:": "esgEdge",
    "Reachable ESG nodes:": "reachableEsgNode",
    "Reachable ESG edges:": "reachableEsgEdge",
}

# Chain-count lines in the analyzer's `--- Total ---` block. Anchored on the (N) prefix so the
# A2S2K/A2S2C lines (6)/(7) do not shadow the H2S2K/H2S2C lines (2)/(4).
state_dep_metrics = {
    "(2) DATA writes to key:":        "H2S2K",
    "(4) DATA writes to cond:":       "H2S2C",
    "(6) A2S2K DATA writes to key:":  "A2S2K",
    "(7) A2S2C DATA writes to cond:": "A2S2C",
    "(1) non-write SO reads:":        "nonWriteReads",
}

# Register-metric column keys that are meaningful at 0 and must survive the zero-prune.
PROTECTED_KEYS = {"H2S2K", "H2S2C", "A2S2K", "A2S2C",
                  "candidate_registers", "D_all", "D_crao", "D_rao"}
PROTECTED_PREFIXES = ("reg_", "frac_")

def extract_result_blocks(lines):
    blocks = []
    block_names = []
    dep_types = []
    current_block = []
    cur_block_name = ""
    cur_dep_type = False
    for line in lines:
        stripped = line.strip().rstrip('\n')
        if re.search(r'\[RESULT\]', stripped):
            parts = stripped.split()
            if 'Action Parameters -> Stateful Variables -> Headers/Keys' in stripped:
                cur_block_name = 'a2s2v_' + parts[-1][:-1] + '.txt'
                cur_dep_type = "A2S2V"

            elif 'Headers -> Stateful Variables -> Headers/Keys' in stripped:
                cur_block_name = 'h2s2v_' + parts[-1][:-1] + '.txt'
                cur_dep_type = "H2S2V"

            elif 'Action Parameters -> Stateful Variables' in stripped:
                cur_block_name = 'a2s_' + parts[-1][:-1] + '.txt'
                cur_dep_type = "A2S"

            elif 'Headers -> Stateful Variables' in stripped:
                cur_block_name = 'h2s_' + parts[-1][:-1] + '.txt'
                cur_dep_type = "H2S"

            elif 'Stateful Variables -> Headers/Keys' in stripped:
                cur_block_name = 's2v_' + parts[-1][:-1] + '.txt'
                cur_dep_type = "S2V"

        elif cur_block_name and all(c == '=' for c in stripped):
            if len(current_block) > 0:
                blocks.append(current_block)
                block_names.append(cur_block_name)
                dep_types.append(cur_dep_type)

            current_block = []
            cur_block_name = ""
            cur_dep_type = False

        elif cur_block_name:
            current_block.append(line.rstrip('\n'))
    return blocks, block_names, dep_types

_SO_LINE = re.compile(r'SOs\[([^\]]+)\]:\s*(.*)')


def _compute_reg_columns(sets, types):
    """Derive all distinct-SO (register) count columns + fractions by set algebra over the
    per-relation sets (`sets`) and per-type sets (`types`). Adding a combination here needs no
    analyzer rebuild. Per-type denominators appear only once the analyzer emits SOs[TYPE:*]."""
    S = lambda k: sets.get(k, set())
    T = lambda t: types.get(t, set())
    D_all = set().union(*types.values()) if types else S("ALL")
    affected = {
        "reg_A2S": S("A2S"), "reg_H2S": S("H2S"),
        "reg_A2S2K": S("A2S2K"), "reg_A2S2C": S("A2S2C"),
        "reg_H2S2K": S("H2S2K"), "reg_H2S2C": S("H2S2C"),
        "reg_A2S_or_H2S": S("A2S") | S("H2S"), "reg_A2S_and_H2S": S("A2S") & S("H2S"),
        "reg_A2S_chain": S("A2S2K") | S("A2S2C"), "reg_H2S_chain": S("H2S2K") | S("H2S2C"),
        "reg_K_anysrc": S("A2S2K") | S("H2S2K"), "reg_C_anysrc": S("A2S2C") | S("H2S2C"),
        "reg_K_bothsrc": S("A2S2K") & S("H2S2K"), "reg_C_bothsrc": S("A2S2C") & S("H2S2C"),
        "reg_any_chain": S("A2S2K") | S("A2S2C") | S("H2S2K") | S("H2S2C"),
    }
    out = {k: len(v) for k, v in affected.items()}
    out["candidate_registers"] = len(D_all)
    out["D_all"] = len(D_all)
    denoms = {"all": D_all}
    if types:
        denoms["crao"] = T("Counter") | T("Register") | T("RegisterAction") | T("AddOnMiss")
        denoms["rao"] = T("Register") | T("RegisterAction") | T("AddOnMiss")
        out["D_crao"] = len(denoms["crao"])
        out["D_rao"] = len(denoms["rao"])
    for dn, D in denoms.items():
        out[f"frac_A2SorH2S_{dn}"] = (round(len(affected["reg_A2S_or_H2S"] & D) / len(D), 6)
                                     if D else 0.0)
        out[f"frac_anychain_{dn}"] = (round(len(affected["reg_any_chain"] & D) / len(D), 6)
                                     if D else 0.0)
    return out


def extract_state_dep_counts(lines):
    """Extract chain counts + register(SO)-set metrics from the analyzer's Total section."""
    stats = {}
    sets = {}    # relation -> {SO names}
    types = {}   # extern type -> {SO names}
    in_total_section = False

    for line in lines:
        # Check if we're in the Total section
        if '--- Total ---' in line:
            in_total_section = True
            continue
        if not in_total_section:
            continue
        # Stop at equals line (end of block)
        if '=========' in line:
            break

        # SOs[<REL>]: / SOs[TYPE:<T>]: name name ...  (clean dotted SO names, whitespace-separated)
        m = _SO_LINE.search(line)
        if m:
            label, rest = m.group(1), m.group(2)
            names = {n.rstrip(';') for n in rest.split() if n.strip(';')}
            if label.startswith("TYPE:"):
                types[label[5:]] = names
            else:
                sets[label] = names
            continue

        # Chain-count lines.
        for pattern, key in state_dep_metrics.items():
            if pattern in line:
                matched = re.search(rf'{re.escape(pattern)}\s*(\d+)', line)
                if matched:
                    stats[key] = int(matched.group(1))
                    break

    stats.update(_compute_reg_columns(sets, types))
    return stats

def analyze_log(inputfile, outdir):
    with open(inputfile) as f:
        content = f.readlines()

    blocks, block_names, dep_types = extract_result_blocks(content)
    stats = Counter()

    # Extract State Dependency Counts
    state_dep_stats = extract_state_dep_counts(content)
    stats.update(state_dep_stats)

    if len(blocks) == 0:
        print(f'No found blocks for {inputfile}')
        return [], stats

    Path(outdir).mkdir(exist_ok=True)
    for i, block in enumerate(blocks):
        filename = f'{outdir}/{block_names[i]}'
        with open(filename, 'w') as f:
            f.writelines(f'{l}\n' for l in block)

        prefixKey = dep_types[i]
        for _, key in pattern_to_key.items():
            statKey = f'{prefixKey}_{key}'
            if statKey not in stats:
                stats[f'{prefixKey}_{key}'] = 0

        for line in block:
            # Count pattern
            for pattern, key in pattern_to_key.items():
                if pattern in line:
                    stats[f'{prefixKey}_{key}'] += 1

            # Parse counts
            for pattern, key in graph_metric_to_key.items():
                if line.startswith(pattern):
                    matched = re.search(rf'{re.escape(pattern)}\s*(\d+)', line)
                    if matched:
                        stats[f'{prefixKey}_{key}'] += int(matched.group(1))
                        break
    for k, v in list(stats.items()):
        if v == 0 and k not in PROTECTED_KEYS and not k.startswith(PROTECTED_PREFIXES):
            del stats[k]

    print(f'Extracted {len(blocks)} blocks to {outdir}/ *.txt [code_file:1]')
    return block_names, stats

# CLI
def main():
    parser = argparse.ArgumentParser(description='Extract [RESULT] to === blocks')
    parser.add_argument('--input', nargs='?', required=True, help='Input file (default: stdin)')
    args = parser.parse_args()

    analyze_log(args.input, 'output')

if __name__ == "__main__":
    main()
