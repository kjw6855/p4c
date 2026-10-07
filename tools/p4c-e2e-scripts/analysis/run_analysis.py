#!/usr/bin/env python3
import argparse
import re
import os, json, csv
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
import multiprocessing
import sys
from common import home_to_tilde
from count_files import count_files, clear_count_file_set
from read_log import analyze_log

# Reuse the canonical program-list parser from the p4c-e2e-scripts root (handles the 5-col
# `name path target arch ver` format + symbex_timeout tag, and unifies the program-name key).
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
from tampering import parse_program_list

HOME = os.path.expanduser("~")
CWD = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(HOME, "Workspace-remote", "usenix27", "metrics")
P4C_ROOT = os.path.join(HOME, "Workspace", "p4c")
P4C_BINARY = os.path.join(P4C_ROOT, "build", "p4c")
P4C_ANALYSIS_BINARY = os.path.join(P4C_ROOT, "build", "p4c_state_dependency")
P4C_METRICS_BINARY = os.path.join(P4C_ROOT, "build", "p4c-metrics")
TOFINO_INCLUDE = os.path.join(P4C_ROOT, "backends", "tofino", "bf-p4c", "p4include")
P4_INCLUDE = os.path.join(P4C_ROOT, "p4include")
# Per-program wall-clock cap on p4c_state_dependency: the IFDS analysis can hang on heavy programs
# (fabric, Ripple, DART, Mew, FlyMon, HorusEye, ChameleMon, fisslock). Overridable via env.
PER_PROG_TIMEOUT = int(os.environ.get("RUN_ANALYSIS_TIMEOUT", "1800"))
BASE_P4C_ANALYSIS_ARGS = [
    #"-T", "stateful_to_key.cpp:2",
    #"-T", "tabulation.cpp:5",
    "--std", "p4-16",
    "--showVar", "REACHABLE",
    "--supergraph", "FULL",
]
BASE_P4C_METRICS_ARGS = [
    "--custom-metrics", "all"
]
REG_FUNC_NAMES = [
    "RegisterAction.execute",
    "register.write",
    "register.read",
    "Counter.count",
    "Meter.meter"
]

CSV_NAME_KEY = "name"
CSV_TARGET_KEY = "target"
CSV_VER_KEY = "std"
CSV_ERR_KEY = "error"
CSV_ARCH_KEY = "arch"
columns = []        # global/outer scope
rows = []           # buffered dicts

def make_row(row_name, target_name, p4ver, arch_name, metrics, err_msg):
    row = {
        CSV_NAME_KEY: row_name,
        CSV_TARGET_KEY: target_name,
        CSV_VER_KEY: p4ver,
        CSV_ARCH_KEY: arch_name,
        CSV_ERR_KEY: err_msg,
    }
    row.update(metrics)
    return row

def add_row(d):
    global columns, rows
    # update columns with any new keys in this row, preserving order
    for k in d.keys():
        if k not in columns:
            columns.append(k)
    rows.append(d)

def flush_csv(path):
    global columns, rows

    # Remove if present so we can control their positions
    cols = [c for c in columns if c not in (CSV_NAME_KEY, CSV_ERR_KEY)]

    # Rebuild with fixed first/last positions
    fieldnames = [CSV_NAME_KEY] + cols + [CSV_ERR_KEY]

    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for d in rows:
            w.writerow(d)

def get_analyzer_args(target: str, arch: str):
    """Frontend args for (target, arch): tofino target gets --target tofino + the bf-p4c includes;
    every target gets the p4c p4include + its --arch. Handles the tofino/v1model composite."""
    args = []
    if target == "tofino":
        args += ["-D__TARGET_TOFINO__=1", "--target", "tofino", "-I", TOFINO_INCLUDE]
    args += ["-I", P4_INCLUDE, "--arch", arch]
    return args

def prompt(prompt_text="> "):
    try:
        return input(prompt_text)
    except KeyboardInterrupt:
        # Ctrl-C
        print("\nInterrupted by user, exiting gracefully.")
        raise SystemExit(130)
    except EOFError:
        # Ctrl-D (or Ctrl-Z+Enter on Windows)
        print("\nEnd of input, exiting gracefully.")
        raise SystemExit(0)

def receive_user_input():
    while True:
        line = prompt("Do you want to continue? (yes/no): ").lower()
        if not line:
            continue
        if line.strip() in {"quit", "exit"}:
            print("Bye.")
            break
        if line.strip() in {"y", "yes"}:
            return True
        if line.strip() in {"n", "no"}:
            return False
    return False;

def load_metrics(path):
    with open(path) as f:
        data = json.load(f)

    num_tables = data["match_action_tables"]["num_tables"]
    total_actions = data["match_action_tables"]["total_actions"]

    num_actions_with_param = data["action_parameters"]["num_actions_with_parameter"]
    total_parameters = data["action_parameters"]["total_parameters"]

    extern_func_uses = data["extern"]["per_function_uses"]

    register_uses = {k: v for k, v in extern_func_uses.items() if any(n == k for n in REG_FUNC_NAMES)}

    metrics_result = {
        "num_tables": num_tables,
        "total_actions": total_actions,
        "num_actions_with_parameter": num_actions_with_param,
        "total_parameters": total_parameters,
    }

    metrics_result.update(register_uses)

    return metrics_result

def parse_perf_data(path):
    pattern = r'([^\s:]+):\s*(\d+)\s*ms'
    perf_data = {}
    with open(path, 'r') as fp:
        parse_mode = False
        for count, line in enumerate(fp):
            if line.strip().startswith('============ Timers ============'):
                parse_mode = True
            if not parse_mode:
                continue

            matched = re.match(pattern, line.strip())
            if matched:
                component = matched.group(1)
                time_ms = int(matched.group(2))
                perf_data[f'{component} (ms)'] = time_ms

    return perf_data

def process_file(p4name: str, targetfile: str, target: str, arch: str, p4ver: str,
                 skipAnalysis: bool, verbose: bool, count_only: bool, p4c_args):
    print(f"Analyzing {p4name} ...")
    commondir = Path(os.path.join(OUTPUT_DIR, p4name)).resolve()
    logdir = Path(os.path.join(OUTPUT_DIR, p4name, "logs")).resolve()
    irdir = Path(os.path.join(OUTPUT_DIR, p4name, "ir")).resolve()
    graphdir = Path(os.path.join(OUTPUT_DIR, p4name, "graphs")).resolve()
    p4srcdir = Path(os.path.join(OUTPUT_DIR, p4name, "p4src")).resolve()

    targetPath = Path(targetfile).resolve()

    commondir.mkdir(parents=True, exist_ok=True)
    logdir.mkdir(exist_ok=True)
    irdir.mkdir(exist_ok=True)
    graphdir.mkdir(exist_ok=True)
    p4srcdir.mkdir(exist_ok=True)

    resultDict = {}

    includeDirs = []
    p4c_args_without_I = []
    doInclude = False
    givenFlags = []
    for i, p4arg in enumerate(p4c_args):
        if p4arg.startswith("~"):
            p4c_args[i] = os.path.expanduser(p4arg)

        if p4arg == "-I":
            doInclude = True
            continue
        if p4arg.startswith("-D"):
            givenFlags.append(p4arg[2:])
            continue
        if doInclude:
            includeDirs.append(p4c_args[i])
            doInclude = False
        else:
            p4c_args_without_I.append(p4c_args[i])

    BASE_P4C_ARGS = get_analyzer_args(target, arch) + p4c_args
    # 0. preprocess to make it as 16
    if p4ver == "14":
        p416_file = p4srcdir / f"{targetPath.name[:-3]}.p4"
        p4cTo16Cmd = [P4C_BINARY] + BASE_P4C_ARGS + \
                ["--std", "14", "--pp", str(p416_file), str(os.path.expanduser(targetfile))]
        p4cTo16log_path = logdir / f"{targetPath.name[:-3]}_14to16_result.txt"
        p4cTo16log_relpath = home_to_tilde(p4cTo16log_path)

        if verbose:
            print("Execute: " + " ".join(p4cTo16Cmd))
        if not skipAnalysis or not os.path.isfile(p416_file):
            try:
                with p4cTo16log_path.open("w") as out:
                    proc = subprocess.run(
                        p4cTo16Cmd,
                        stdout=out,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )
                if proc.returncode != 0:
                    # Conversion sometimes works well without any log
                    hasError = True
                    logline = 0
                    with p4cTo16log_path.open("r") as fp:
                        for line in fp:
                            line = line.strip()
                            if line.startswith("0 error"):
                                hasError = False
                            if len(line) > 0:
                                logline += 1
                    if hasError and logline > 0:
                        return resultDict, f"[FAILED:14-to-16] {targetPath.name} (log: {p4cTo16log_relpath})"
                    else:
                        print(f"[WARNING:14-to-16] {targetPath.name} (log: {p4cTo16log_relpath})", flush=True)
                else:
                    print(f"[SUCCESS] {targetPath.name} (log: {p4cTo16log_relpath})", flush=True)
            except Exception as e:
                return resultDict, f"{targetPath.name} failed with exception: {e}"

        # Update P4_14 with include to merged P4_16 file
        targetfile = p416_file
        p4c_args = p4c_args_without_I

    if verbose:
        print(f"target file: {targetfile}")

    # 1. count lines of code: loc
    clear_count_file_set()
    resultDict["lines_of_code"], resultDict["lines_of_code_without_const_entry"] = count_files(targetfile, givenFlags, includeDirs, verbose)

    if count_only:
        return resultDict, ""

    BASE_P4C_ARGS = get_analyzer_args(target, arch) + p4c_args

    '''
    # TODO: Use P4C-IR to reduce time. Currently P4C can give more errors because of P4 program bugs.
    # 2. get IR
    irJsonPath = irdir / f"{targetPath.name[:-3]}.json"
    irJsonRelPath = home_to_tilde(irJsonPath)
    irLogPath = logdir / f"{targetPath.name[:-3]}_p4c_ir.log"
    irLogRelPath = home_to_tilde(irLogPath)
    irCmd = [P4C_BINARY] + BASE_P4C_ARGS + [str(os.path.expanduser(targetfile)), "--ir-to-json", str(irJsonPath)]
    if verbose:
        print("Execute: " + " ".join(irCmd))
    if not skipAnalysis or not os.path.isfile(irJsonPath):
        try:
            with irLogPath.open("w") as out:
                proc = subprocess.run(
                    irCmd,
                    stdout=out,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
            if proc.returncode != 0:
                return resultDict, f"[FAILED:p4c-ir] {targetPath.name} (log: {irLogRelPath})"
            else:
                print(f"[SUCCESS] {targetPath.name}: {irJsonRelPath}", flush=True)
        except Exception as e:
            return resultDict, f"{targetPath.name} failed in running p4c with exception: {e}"
    '''

    # 2. get metrics by running p4c-metrics
    metricCmd = [P4C_METRICS_BINARY] + BASE_P4C_METRICS_ARGS + BASE_P4C_ARGS +\
            [str(targetfile), "--out-dir", str(logdir)]
    metricJsonPath = logdir / f"{targetPath.name[:-3]}_metrics.json"
    metricJsonRelPath = home_to_tilde(metricJsonPath)
    metricLogPath = logdir / f"{targetPath.name[:-3]}_metrics.log"
    metricLogRelPath = home_to_tilde(metricLogPath)

    if verbose:
        print("Execute: " + " ".join(metricCmd))
    if not skipAnalysis or not os.path.isfile(metricJsonPath):
        try:
            with metricLogPath.open("w") as out:
                proc = subprocess.run(
                    metricCmd,
                    stdout=out,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
            if proc.returncode != 0:
                return resultDict, f"[FAILED:p4c-metrics] {targetPath.name} (log: {metricLogRelPath})"
            else:
                print(f"[SUCCESS] {targetPath.name}: {metricJsonRelPath}", flush=True)
        except Exception as e:
            return resultDict, f"{targetPath.name} failed in running p4c-metrics with exception: {e}"

    resultDict.update(load_metrics(metricJsonPath))
    if verbose:
        print(f"[{p4name}] {resultDict}")

    # 3. run analysis
    cmd = [P4C_ANALYSIS_BINARY] + BASE_P4C_ANALYSIS_ARGS + BASE_P4C_ARGS +\
            [str(targetfile), "--graphs-dir", str(graphdir), "--print-performance-report"]
    result_path = logdir / f"{targetPath.name[:-3]}_result.txt"
    result_relpath = home_to_tilde(result_path)

    if verbose:
        print(" ".join(cmd) + f" >{result_relpath} 2>&1")

    # (skipAnalysis && hasFile)
    if not skipAnalysis or not os.path.isfile(result_path):
        try:
            with result_path.open("w") as out:
                proc = subprocess.run(
                    cmd,
                    stdout=out,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=PER_PROG_TIMEOUT,
                )
            if proc.returncode != 0:
                return resultDict, f"[FAILED:p4c-sd] {targetPath.name} (log: {result_relpath})"
            else:
                print(f"[SUCCESS] {targetPath.name} (log: {result_relpath})", flush=True)
        except subprocess.TimeoutExpired:
            return resultDict, f"[TIMEOUT:p4c-sd] {targetPath.name} (>{PER_PROG_TIMEOUT}s, log: {result_relpath})"
        except Exception as e:
            return resultDict, f"{targetPath.name} failed with exception: {e}"

    if not os.path.isfile(result_path):
        return resultDict, f"No analysis result: {result_path}"

    _, analyzeStats = analyze_log(result_path, logdir)
    resultDict.update(analyzeStats)

    perfStats = parse_perf_data(result_path)
    resultDict.update(perfStats)

    return resultDict, ""

from concurrent.futures import wait, FIRST_COMPLETED

def wait_some(futures, drain=False):
    if not futures:
        return set()
    if drain:
        done, _ = wait(futures)  # wait for all remaining
    else:
        done, _ = wait(futures, return_when=FIRST_COMPLETED)
    return done


def handle_done(done_futures, futures_dict, verbose=False):
    out = []
    for fut in done_futures:
        ctx = futures_dict.pop(fut, None)
        try:
            metrics, errMsg = fut.result()
            if verbose:
                print(errMsg)
            out.append(make_row(ctx["p4name"], ctx["target"], ctx["p4ver"], ctx["arch"], metrics, errMsg))
        except Exception as e:
            # log or handle error
            out.append(make_row(ctx["p4name"], ctx["p4ver"], ctx["arch"], {}, str(e)))
    return out

def main():
    parser = argparse.ArgumentParser(
        description="Python translation of run_analysis.sh"
    )
    parser.add_argument(
        "--max-jobs",
        type=int,
        default=min(6, multiprocessing.cpu_count()),
        help="Maximum parallel jobs (default: min(6, #CPUs); capped to share the host with the "
             "running nightly sweep + tofino VM)",
    )
    parser.add_argument("--yes", "-y", action='store_true', help="Proceed anyway")
    parser.add_argument("--input", help="Input file of list of target programs", required=True)
    parser.add_argument("--target", "-t", action="append",
            help="Specify target P4 program file(s)")
    parser.add_argument("--output", help="Output csv file of analysis", required=True)
    parser.add_argument("--skip-analysis", "-s", action='store_true',
            help="Skip running p4c_state_dependency for analysis")
    parser.add_argument("--use-ir", "-r", action='store_true',
            help="Use IR instead of pure P4 program")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--count-only", "-c", action="store_true",
            help="Only count lines of code without running analysis")
    args = parser.parse_args()

    # Collect tasks via the canonical parser over every (target, arch) combo in scope. Each row is
    # one (program x target x arch); parse_program_list filters to one combo, so union the four.
    MATRIX = [("bmv2", "v1model"), ("tofino", "tna"), ("tofino", "v1model"), ("dpdk", "pna")]
    specs = []
    for tgt, arch in MATRIX:
        specs += parse_program_list(Path(args.input), tgt, arch)
    if args.target:  # optional substring filter on program name
        specs = [s for s in specs if any(t in s.name for t in args.target)]

    if not specs:
        print("No valid P4 programs found.")
        return

    for index, s in enumerate(specs):
        print(f"[{index+1}] {s.target}/{s.arch} {s.name} {s.p4_version}: {s.p4_file}")

    if not args.yes and not receive_user_input():
        return

    max_jobs = args.max_jobs
    total_tasks = len(specs)
    completed_tasks = 0

    # Parallel execution, similar to bash background jobs + wait -n
    futures = {}
    results = []
    with ProcessPoolExecutor(max_workers=max_jobs) as executor:
        for s in specs:
            if len(futures) >= max_jobs:
                done = wait_some(futures)
                new_rows = handle_done(done, futures, args.verbose)
                results.extend(new_rows)
                for row in new_rows:
                    completed_tasks += 1
                    has_err = bool(row.get(CSV_ERR_KEY))
                    status = "FAILED" if has_err else "DONE"
                    print(
                        f"[PROGRESS] {completed_tasks}/{total_tasks} ({completed_tasks * 100 / total_tasks:.1f}%) "
                        f"{status} {row[CSV_ARCH_KEY]} {row[CSV_NAME_KEY]}"
                    )

            fut = executor.submit(process_file, s.name, s.p4_file, s.target, s.arch, s.p4_version,
                    args.skip_analysis, args.verbose, args.count_only, list(s.extra_args))

            futures[fut] = {
                "p4name": s.name,
                "fpath": s.p4_file,
                "target": s.target,
                "arch": s.arch,
                "p4ver": s.p4_version,
            }

        done = wait_some(futures, drain=True)
        new_rows = handle_done(done, futures, args.verbose)
        results.extend(new_rows)
        for row in new_rows:
            completed_tasks += 1
            has_err = bool(row.get(CSV_ERR_KEY))
            status = "FAILED" if has_err else "DONE"
            print(
                f"[PROGRESS] {completed_tasks}/{total_tasks} ({completed_tasks * 100 / total_tasks:.1f}%) "
                f"{status} {row[CSV_ARCH_KEY]} {row[CSV_NAME_KEY]}"
            )

    for r in results:
        add_row(r)
    flush_csv(args.output)

if __name__ == "__main__":
    main()
