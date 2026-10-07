#!/usr/bin/env python3
import argparse
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import multiprocessing


HOME = os.path.expanduser("~")
OUTPUT_DIR = os.path.join(HOME, "Workspace-remote", "usenix27", "collect")
LOG_DIR = os.path.join(OUTPUT_DIR, "logs")
P4C_ANALYSIS_BINARY = os.path.join(HOME, "Workspace", "p4c", "build", "p4c_state_dependency")
BASE_P4C_ARGS = [
    "-T", "stateful_to_key.cpp:2",
    "-T", "tabulation.cpp:5",
    "--showVar", "REACHABLE",
    "--supergraph", "FULL",
]


def detect_target_and_args(p4_file: Path):
    """
    Mimic the grep checks in the bash script to determine target and extra args.
    Note: This reads the file once instead of running multiple greps.
    """
    text = p4_file.read_text(errors="ignore")
    target = None
    extra_args = []

    if "tna.p4" in text:
        target = "tna"
        extra_args += ["-D__TARGET_TOFINO__=1", "--target", "tofino", "--arch", "tna"]
    if "v1model.p4" in text:
        target = "v1model"
        extra_args += ["--target", "bmv2", "--arch", "v1model"]
    if "pna.p4" in text:
        target = "pna"
        extra_args += ["--arch", "pna"]
    if "psa.p4" in text:
        target = "psa"
        extra_args += ["--arch", "psa"]

    if target:
        return target, extra_args, "16"

    if "tofino/intrinsic_metadata.p4" in text:
        return "tna", ["--target", "tofino", "--arch", "tna"], "14"
    if "control ingress" in text:
        return "v1model", ["--target", "bmv2", "--arch", "v1model"], "14"

    return target, extra_args, ""

def home_to_tilde(path: str) -> str:
    path = str(path)
    if path == str(HOME):
        return "~"
    if path.startswith(str(HOME) + "/"):
        return "~" + path[len(str(HOME)):]
    return path

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

def process_file(targetfile: Path, graphdir: Path, logdir: Path, p4c_args: list[str]):
    print(f"Store log in {logdir}")
    print(f"Store graphs in {graphdir}")
    logdir.mkdir(parents=True, exist_ok=True)
    graphdir.mkdir(parents=True, exist_ok=True)

    cmd = [P4C_ANALYSIS_BINARY] + p4c_args + [str(targetfile), "--graphs-dir", str(graphdir)]
    result_path = logdir / f"{targetfile.name}_result.txt"
    result_relpath = home_to_tilde(result_path)
    print(" ".join(cmd) + f" >{result_relpath} 2>&1")

    try:
        with result_path.open("w") as out:
            proc = subprocess.run(
                cmd,
                stdout=out,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if proc.returncode != 0:
            print(f"[FAILED] {targetfile.name} (log: {result_relpath})", flush=True)
        else:
            print(f"[SUCCESS] {targetfile.name} (log: {result_relpath})", flush=True)
    except Exception as e:
        print(f"{targetfile.name} failed with exception: {e}", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description="Python translation of run_analysis.sh"
    )
    parser.add_argument(
        "--max-jobs",
        type=int,
        default=multiprocessing.cpu_count(),
        help="Maximum parallel jobs (default: number of CPUs)",
    )
    parser.add_argument("--yes", "-y", action='store_true', help="Proceed anyway")
    args, passthrough = parser.parse_known_args()

    if passthrough:
        args.input_dir = passthrough[0]
    else:
        args.input_dir = "./"

    # Resolve and normalize dir, basedir
    dir_path = Path(args.input_dir).resolve()

    basedir = ""
    if args.input_dir != "." and args.input_dir != "./":
        basedir = dir_path.name


    # Find all .p4 files (like grep -rl --include='*.p4' '' dir)
    p4_files = list(dir_path.rglob("*.p4"))

    if not p4_files:
        return

    # Collect tasks
    for f in p4_files:
        if os.path.isdir(f):
            continue
        base = f.name
        # rel: path relative to dir_path
        rel = f.relative_to(dir_path)
        rel_str = str(rel)

        parent_underscored = ""
        # If the file is not directly under dir_path, compute parent path with underscores
        if base != rel_str:
            parent = str(rel.parent)
            parent_underscored = parent.replace("/", "_")

        target, extra_args, p4ver = detect_target_and_args(f)
        if not target:
            continue

        # Build P4C args; note this mimics the bash script mutation behavior

        p4name = parent_underscored + "_" + base[:-3]
        if basedir:
            p4name = basedir + "_" + p4name
        fpath = home_to_tilde(f)
        print(f"{p4name} {fpath} {target} {p4ver}")

if __name__ == "__main__":
    main()
