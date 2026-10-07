#!/usr/bin/env python3
"""Replay p4symbex three-phase tampering test cases on a live BMv2 dataplane.

For every v1model entry in ~/Workspace-remote/top_tier_repo/program_list.txt the
script:
  1. Builds the P4 program with p4c (skipped if outputs exist).
  2. Re-runs p4symbex with --path-selection STATE_DEP_TAMPERING (unless
     --skip-p4symbex).
  3. Starts simple_switch_grpc on veth0..veth14 and installs the pipeline.
  4. For each generated *.txtpb: clears tables, installs entities from the
     file, sends the three input packets through Scapy, and verifies phase 1
     and phase 3 expected outputs (strict).
  5. Writes one CSV row per protobuf into
     <output-root>/test_bmv2_tampering_results.csv.

Run with: sudo -v && python3 test_bmv2_tampering.py
"""

from __future__ import annotations

import json
import argparse
import codecs
import csv
import logging
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

# BMv2-only control-plane / packet dependencies (P4Runtime bindings, scapy, the BMv2
# clients) are NOT imported at module load: a Tofino-only environment may not have them
# installed, and importing eagerly would break `--target tofino`. They are loaded on
# demand by load_bmv2_deps() and published as module globals so every function
# (main, do_testing, bmv2_session, the entity helpers) can use them. All type
# annotations referencing these names are strings thanks to `from __future__ import
# annotations` (top of file), so they are never evaluated at import time.
P4RuntimeClient = None
P4RuntimeWriteException = None
Bmv2ThriftClient = None
p4runtime_pb2 = None
p4data_pb2 = None
p4info_pb2 = None
text_format = None
Ether = None
sendp = None
rdpcap = None
scapy_conf = None


def load_bmv2_deps() -> None:
    """Import BMv2 control-plane dependencies and publish them as module globals.

    Called once before any BMv2 testing work. Safe to call multiple times. Never called
    on the Tofino path, so Tofino-only environments need none of these packages.

    IMPORTANT: this script is loaded twice when run directly — once as `__main__`
    (the entry point) and once as `tampering` (re-imported by builders.py / bmv2_driver.py
    so do_testing & friends are callable). Plain `global` here would only populate the
    namespace of whichever copy calls this. do_testing resolves its names against the
    `tampering` module, so we publish into BOTH this module's globals and the canonical
    `tampering` module's namespace to keep the two copies consistent.
    """
    if P4RuntimeClient is not None:  # already loaded
        return
    try:
        from p4.v1 import p4runtime_pb2 as _p4rt
        from p4.v1 import p4data_pb2 as _p4data
        from p4.config.v1 import p4info_pb2 as _p4info
        from google.protobuf import text_format as _text_format
    except ImportError as e:
        sys.exit(f"missing P4Runtime python bindings: {e}")
    try:
        from scapy.all import Ether as _Ether, sendp as _sendp, rdpcap as _rdpcap, \
            conf as _scapy_conf
        _scapy_conf.verb = 0
    except ImportError as e:
        sys.exit(f"missing scapy: {e}")
    try:
        from bmv2.p4_client import P4RuntimeClient as _Client, \
            P4RuntimeWriteException as _WriteExc
        from bmv2.bmv2_thrift_client import Bmv2ThriftClient as _Thrift
    except ImportError as e:
        sys.exit(f"cannot import bmv2 control-plane client (expected under "
                 f"{Path(__file__).parent}/bmv2/): {e}")

    loaded = {
        "p4runtime_pb2": _p4rt, "p4data_pb2": _p4data, "p4info_pb2": _p4info,
        "text_format": _text_format,
        "Ether": _Ether, "sendp": _sendp, "rdpcap": _rdpcap, "scapy_conf": _scapy_conf,
        "P4RuntimeClient": _Client, "P4RuntimeWriteException": _WriteExc,
        "Bmv2ThriftClient": _Thrift,
    }
    # Publish the names into the exact namespace the consuming functions resolve against.
    # When run as `python3 ./tampering.py` this script lives in sys.modules twice (as
    # "__main__" and as "tampering", re-imported by builders.py); a name set on one copy is
    # invisible to functions defined in the other. do_testing.__globals__ IS the module dict
    # that do_testing and all its sibling functions (load_p4info, install_entities,
    # _sudo_sendp, ...) look names up in, so writing there reaches every consumer regardless
    # of which module copy defined them. Also mirror onto this function's own globals so a
    # direct caller in the entry-point copy sees them too.
    # Update this file's namespace in BOTH possible sys.modules slots. When run as
    # `python3 ./tampering.py`, the entry point is the "__main__" copy (where this function
    # runs), but builders.py / bmv2_driver.py do `from tampering import ...`, creating a
    # SEPARATE "tampering" copy whose functions resolve names against ITS own dict. Updating
    # only our own globals() (the __main__ copy) leaves the tampering copy's do_testing with
    # None. Import the canonical "tampering" module and update its dict too.
    namespaces = [globals()]
    try:
        import importlib
        canonical = importlib.import_module("tampering")
        if canonical.__dict__ is not globals():
            namespaces.append(canonical.__dict__)
    except Exception:
        pass
    for ns in namespaces:
        ns.update(loaded)

# Hardcoded so that running under `sudo` (which sets HOME=/root) still resolves
# the well-known paths correctly.
HOME = Path("/home/vagrant")

# Per-target subpackages live next to this script:
#   bmv2/p4_client.py, bmv2/bmv2_thrift_client.py   — BMv2 control plane
#   tofino/bfrt_grpc_client.py, tofino/tofino_driver.py — Tofino control plane
sys.path.insert(0, str(Path(__file__).resolve().parent))
# Binary locations are per-VM (this VM builds open-source p4c; a Tofino VM builds
# bf-p4c with its own p4symbex), so they are overridable via the environment. The
# defaults match a stock ~/Workspace/p4c build; set $P4C_BIN / $P4SYMBEX_BIN (or the
# --p4c-bin / --p4symbex-bin flags) on VMs whose binaries live elsewhere.
P4C_BIN = Path(os.environ.get("P4C_BIN") or (HOME / "Workspace/p4c/build/p4c"))
P4SYMBEX_BIN = Path(os.environ.get("P4SYMBEX_BIN") or (HOME / "Workspace/p4c/build/p4symbex"))
SIMPLE_SWITCH_GRPC = shutil.which("simple_switch_grpc") or "simple_switch_grpc"

GRPC_HOST = "127.0.0.1"  # localhost may resolve to ::1 only; BMv2 listens IPv4
GRPC_PORT = 9559
THRIFT_PORT = 9090
DEVICE_ID = 0  # simple_switch_grpc defaults to device_id 0
ELECTION_ID = (0, 1)

# Switch sees veth{2N} (even); host injects/sniffs on veth{2N+1} (odd).
NUM_PORTS = 8
# Minimum packet size (bytes) used as the lower bound of p4symbex's --packet-size-range AND
# as the validity floor when reading input_packet blocks from generated .txtpb files.
# 14 = one Ethernet header; below this scapy's Ether() cannot dissect the frame and replay
# fails. Single source of truth so the generator bound and the replay check never diverge.
GLOBAL_MIN_PACKET_SIZE = 14

# State-dependency tampering policies, selected via --tamper-mode. Each policy is a distinct
# p4symbex --path-selection that mines a different chain set (Write-Key vs Write-Condition), so a
# program may yield tests under one and nothing under the other. Generated into per-policy
# subdirs of the program's txtpb dir to keep filenames from colliding (both start at chain 0).
TAMPER_MODE_POLICIES = {
    "key": ["STATE_DEP_TAMPERING"],
    "cond": ["STATE_DEP_TAMPERING_COND"],
    "both": ["STATE_DEP_TAMPERING", "STATE_DEP_TAMPERING_COND"],
}

PORT_TO_HOST_IFACE = {n: f"veth{2 * n + 1}" for n in range(NUM_PORTS)}
ALL_HOST_IFACES = list(PORT_TO_HOST_IFACE.values())
SWITCH_IFACE_ARGS: List[str] = []
for n in range(NUM_PORTS):
    SWITCH_IFACE_ARGS += ["-i", f"{n}@veth{2 * n}"]

log = logging.getLogger("bmv2_tamper")


# --------------------------------------------------------------------------- #
# Shutdown handling                                                           #
# --------------------------------------------------------------------------- #

SHUTDOWN = threading.Event()
_LIVE_PROCS_LOCK = threading.Lock()
_LIVE_PROCS: List[subprocess.Popen] = []


def _track_proc(proc: subprocess.Popen) -> subprocess.Popen:
    with _LIVE_PROCS_LOCK:
        _LIVE_PROCS.append(proc)
    return proc


def _untrack_proc(proc: subprocess.Popen) -> None:
    with _LIVE_PROCS_LOCK:
        try:
            _LIVE_PROCS.remove(proc)
        except ValueError:
            pass


def _kill_tracked_procs() -> None:
    with _LIVE_PROCS_LOCK:
        procs = list(_LIVE_PROCS)
    if not procs:
        return
    sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
    for p in procs:
        if p.poll() is None:
            try:
                subprocess.run(sudo + ["kill", "-TERM", str(p.pid)],
                               check=False, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL,
                               start_new_session=True, timeout=3)
            except Exception:
                pass
    deadline = time.time() + 1.0
    while time.time() < deadline and any(p.poll() is None for p in procs):
        time.sleep(0.05)
    for p in procs:
        if p.poll() is None:
            try:
                subprocess.run(sudo + ["kill", "-KILL", str(p.pid)],
                               check=False, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL,
                               start_new_session=True, timeout=3)
            except Exception:
                pass


def request_shutdown(reason: str = "") -> None:
    """Signal all workers to stop and forcibly kill our spawned subprocesses."""
    if SHUTDOWN.is_set():
        return
    SHUTDOWN.set()
    if reason:
        # Newline first so we don't clobber the in-place TUI line.
        try:
            sys.stderr.write("\n" + reason + "\n")
            sys.stderr.flush()
        except Exception:
            pass
    # Broad cleanup for the well-known privileged binaries we launch.
    sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
    for name in ("simple_switch_grpc", "tcpdump", "tofino-model", "bf_switchd"):
        try:
            subprocess.run(sudo + ["pkill", "-KILL", "-f", name],
                           check=False, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL,
                           start_new_session=True, timeout=5)
        except Exception:
            pass
    _kill_tracked_procs()


def _sigint_handler(signum, frame):
    # First ^C: request graceful shutdown. Second ^C: hard-exit.
    if SHUTDOWN.is_set():
        os._exit(130)
    request_shutdown(reason="^C received — shutting down workers and subprocesses…")
    # Re-raise so the main thread can unwind out of pipeline.run().
    raise KeyboardInterrupt()


# --------------------------------------------------------------------------- #
# program_list parsing                                                        #
# --------------------------------------------------------------------------- #

@dataclass
class TargetSpec:
    name: str
    p4_file: str
    target: str = "bmv2"
    arch: str = "v1model"
    p4_version: str = "p4-16"
    extra_args: List[str] = field(default_factory=list)
    # Per-program p4symbex per-policy timeout (seconds). Overridden per row via a
    # `symbex_timeout=<N>` token in the program list (auto-computed by categorize_chains.py from
    # chain count). Defaults to the historical 1800s when the row carries no tag.
    timeout: int = 1800


# Backends recognized in the (optional) target column of a program list.
# (dpdk is the target for pna programs; it is a corpus record only — the harness
# drivers support bmv2 and tofino — but it must be recognized so a "dpdk pna" row
# parses as the 5-column format rather than being mistaken for a legacy row.)
_KNOWN_TARGETS = {"bmv2", "tofino", "dpdk"}


# p4symbex budgets above this get `nice -n 10` (see run_p4symbex). A module-level knob rather than a
# literal because --symbex-timeout-min raises it in step: flooring every cheap program to 3h must not
# silently re-prioritise the whole corpus, which would change how the pool schedules without anyone
# asking for it. Anything genuinely above the floor is still niced.
NICE_ABOVE_SECS = 1800


def _apply_timeout_overrides(targets: "List[TargetSpec]", args) -> None:
    """Apply --symbex-timeout-scale / --symbex-timeout-min to every spec, in place.

    Done once here rather than inside parse_program_list because that parser is shared with
    cache_chains.py / run_analysis.py, which have no business inheriting a retry's budget.

    Every adjusted program is logged: a flag that silently did nothing is indistinguishable from one
    that was never read, and that ambiguity is what makes a long retry hard to trust.
    """
    scale = getattr(args, "symbex_timeout_scale", 1.0) or 1.0
    floor = getattr(args, "symbex_timeout_min", 0) or 0
    if scale == 1.0 and floor <= 0:
        return
    # `nice -n 10` keys off the budget (run_p4symbex), so flooring every cheap program to 3h would
    # newly de-prioritise the whole corpus -- a scheduling change nobody asked for. Raise the nice
    # threshold in step so only programs ABOVE the floor stay niced.
    if floor > 0:
        globals()["NICE_ABOVE_SECS"] = max(NICE_ABOVE_SECS, floor)
        try:
            from tofino import tofino_driver as _td
            _td.NICE_ABOVE_SECS = max(_td.NICE_ABOVE_SECS, floor)
        except Exception:                                   # bmv2-only host: nothing to raise
            pass
    changed = 0
    for spec in targets:
        before = spec.timeout
        after = int(round(before * scale))
        if floor > 0:
            after = max(after, floor)
        if after != before:
            spec.timeout = after
            changed += 1
            log.info("[%s] p4symbex budget %ss -> %ss", spec.name, before, after)
    # One line at WARNING so it survives the default log level. INFO (i.e. -v) is where the
    # per-program detail lives, but the headline has to be visible without it: an override that
    # silently did nothing looks exactly like one that was never read, and on a multi-hour retry
    # that ambiguity is expensive to discover late.
    if changed:
        log.warning("p4symbex budget override active (scale=%s, min=%s): %d of %d program(s) "
                    "adjusted; nice threshold now %ss",
                    scale, f"{floor}s" if floor else "none", changed, len(targets),
                    NICE_ABOVE_SECS)


def parse_program_list(path: Path, target: str, arch: str) -> List[TargetSpec]:
    """Return program-list entries matching both @target and @arch.

    Two row formats are accepted:
      new:    <name> <p4_file> <target> <arch> <p4_version> [extra_args...]
      legacy: <name> <p4_file> <arch> <p4_version> [extra_args...]

    The explicit target column lets a v1model program run on the Tofino composite
    target (``--target tofino --arch v1model``) instead of always mapping
    v1model -> bmv2. For legacy rows (no target column) the target is inferred
    from the arch (v1model -> bmv2, tna/t2na -> tofino), preserving old behavior.
    """
    targets: List[TargetSpec] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            toks = line.split()
            if len(toks) < 4:
                continue
            if toks[2] in _KNOWN_TARGETS:
                if len(toks) < 5:
                    continue
                name, p4_file, row_target, row_arch, p4_version = toks[:5]
                rest = toks[5:]
            else:
                name, p4_file, row_arch, p4_version = toks[:4]
                row_target = "bmv2" if row_arch == "v1model" else "tofino"
                rest = toks[4:]
            if row_target != target or row_arch != arch:
                continue
            # Pull the optional `symbex_timeout=<N>` scheduling tag out of the trailing tokens so it
            # is NOT passed to p4c / p4c_state_dependency (which share this parser via cache_chains.py).
            timeout = 1800
            filtered_rest = []
            for t in rest:
                if t.startswith("symbex_timeout="):
                    try:
                        timeout = int(t.split("=", 1)[1])
                    except ValueError:
                        pass
                    continue
                filtered_rest.append(t)
            extras = [t for t in filtered_rest if t != "$@"]
            targets.append(TargetSpec(
                name=name,
                p4_file=os.path.expanduser(p4_file),
                target=row_target,
                arch=row_arch,
                p4_version=p4_version,
                extra_args=[os.path.expanduser(a) for a in extras],
                timeout=timeout,
            ))
    return targets


# --------------------------------------------------------------------------- #
# Build steps                                                                 #
# --------------------------------------------------------------------------- #

class StepError(RuntimeError):
    pass


def _run(cmd: List[str], log_path: Optional[Path] = None, timeout: int = 600) -> None:
    if SHUTDOWN.is_set():
        raise StepError("shutdown requested")
    log.debug("exec: %s", " ".join(str(c) for c in cmd))
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w") as lf:
            proc = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                                  stdout=lf, stderr=subprocess.STDOUT,
                                  start_new_session=True, timeout=timeout)
    else:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True,
                              start_new_session=True, timeout=timeout)
    if proc.returncode != 0:
        if SHUTDOWN.is_set():
            raise StepError("shutdown requested")
        tail = ""
        if log_path is not None and log_path.exists():
            tail = log_path.read_text()[-2000:]
        elif hasattr(proc, "stdout"):
            tail = (proc.stdout or "") + (proc.stderr or "")
        raise StepError(f"command failed ({proc.returncode}): {' '.join(str(c) for c in cmd)}\n{tail[-1000:]}")


def run_p4c(name: str, p4_file: str, extra_args: List[str], build_dir: Path, p4_version: str = "p4-16") -> Tuple[Path, Path]:
    build_dir.mkdir(parents=True, exist_ok=True)
    base = Path(p4_file).stem
    canonical_json = build_dir / f"{name}.json"
    canonical_p4info = build_dir / f"{name}_p4info.txt"
    if canonical_json.exists() and canonical_p4info.exists():
        log.info("[%s] p4c outputs already present, skipping", name)
        return canonical_json, canonical_p4info

    cmd = [
        str(P4C_BIN),
        "--target", "bmv2",
        "--arch", "v1model",
        "--std", p4_version,
        "-o", str(build_dir),
        "--p4runtime-files", str(canonical_p4info),
        *extra_args,
        p4_file,
    ]
    _run(cmd, log_path=build_dir / "p4c.log")

    if not canonical_json.exists():
        emitted = build_dir / f"{base}.json"
        if emitted.exists():
            shutil.copyfile(emitted, canonical_json)
        else:
            jsons = sorted(build_dir.glob("*.json"))
            if not jsons:
                raise StepError(f"p4c produced no .json under {build_dir}")
            shutil.copyfile(jsons[0], canonical_json)
    if not canonical_p4info.exists():
        raise StepError(f"p4c did not produce {canonical_p4info}")
    return canonical_json, canonical_p4info


def run_p4symbex(name: str, p4_file: str, protobuf_dir: Path, *,
                 max_tests: int, tamper_value: str, skip: bool, p4_version: str = "p4-16",
                 extra_args: List[str] = [],
                 path_selection: str = "STATE_DEP_TAMPERING",
                 state_dep_cache: "Optional[Path]" = None,
                 cp_annotation: "Optional[Path]" = None,
                 shared_traversal: "Optional[str]" = None,
                 chain_impact_order: bool = False,
                 timeout: int = 1800) -> List[Path]:
    if skip and protobuf_dir.exists():
        files = sorted(protobuf_dir.glob("*.txtpb"))
        if files:
            log.info("[%s] reusing %d existing txtpb files in %s", \
                     name, len(files), protobuf_dir)
            return files
    if protobuf_dir.exists():
        for f in protobuf_dir.glob("*.txtpb"):
            f.unlink()
    protobuf_dir.mkdir(parents=True, exist_ok=True)

    # Lower CPU priority for expensive (above-default timeout) programs so they don't hog the
    # scheduler when they overlap cheap ones at the tail of the pool.
    nice_prefix = ["nice", "-n", "10"] if timeout > NICE_ABOVE_SECS else []
    cmd = [
        *nice_prefix,
        str(P4SYMBEX_BIN),
        "--target", "bmv2",
        "--std", p4_version,
        "--arch", "v1model",
        "--test-backend", "protobuf",
        # Lower bound = GLOBAL_MIN_PACKET_SIZE so generated packets are always dissectable;
        # the same constant gates the replay-side input_packet check (parse_case).
        "--packet-size-range", f"{GLOBAL_MIN_PACKET_SIZE}:9600",
        "--track-coverage", "STATEMENTS",
        "--max-tests", str(max_tests),
        "--max-port", str(NUM_PORTS),
        "--out-dir", str(protobuf_dir),
        "--state-dep",
        "--path-selection", path_selection,
        *(["--state-dep-cache", str(state_dep_cache)] if state_dep_cache else []),
        *(["--cp-annotation", str(cp_annotation)] if cp_annotation else []),
        # Search-shape options. Neither changes which tests are correct: --shared-traversal only
        # decides which phases share one DFS, and --chain-impact-order only reorders chains. They
        # matter under a per-program timeout, where the run is cut short and chain ORDER decides
        # what got reached (Blink's 53 cond chains and Lemon-jaqen both timed out at 1800s).
        *(["--shared-traversal", shared_traversal] if shared_traversal else []),
        *(["--chain-impact-order"] if chain_impact_order else []),
        *extra_args,
        p4_file,
        "--state-tamper-value", tamper_value,
    ]
    _run(cmd, log_path=protobuf_dir.parent / f"p4symbex.{path_selection}.log", timeout=timeout)
    return sorted(protobuf_dir.glob("*.txtpb"))


# --------------------------------------------------------------------------- #
# BMv2 lifecycle                                                              #
# --------------------------------------------------------------------------- #

class Bmv2Process:
    def __init__(self, json_path: Path, log_path: Path):
        self.json_path = json_path
        self.log_path = log_path
        self.proc: Optional[subprocess.Popen] = None

    def start(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        log_fd = self.log_path.open("w")
        prefix = [] if os.geteuid() == 0 else ["sudo"]
        cmd = [*prefix, SIMPLE_SWITCH_GRPC, "--log-console",
               "--thrift-port", str(THRIFT_PORT),
               *SWITCH_IFACE_ARGS, str(self.json_path)]
        log.info("starting BMv2: %s", " ".join(cmd))
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL, stdout=log_fd, stderr=subprocess.STDOUT,
            preexec_fn=os.setsid,
        )
        _track_proc(self.proc)
        self._wait_for_grpc()
        self._wait_for_thrift()

    def _wait_for_grpc(self, attempts: int = 100, delay: float = 0.2) -> None:
        # Local import: grpc is only needed on the bmv2 replay path (the Tofino path never
        # calls this), and it is not published as a tampering.py module global.
        import grpc
        addr = f"{GRPC_HOST}:{GRPC_PORT}"
        for _ in range(attempts):
            if self.proc and self.proc.poll() is not None:
                raise StepError(
                    f"simple_switch_grpc exited early; see {self.log_path}")
            # Raw TCP first: a cheap reject while the listen socket is not even bound yet.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                try:
                    s.connect((GRPC_HOST, GRPC_PORT))
                except OSError:
                    time.sleep(delay)
                    continue
            # TCP-accept != gRPC-serviceable: simple_switch_grpc binds the listen socket well
            # before its gRPC/HTTP2 stack can serve StreamChannel. Gate on real channel
            # readiness so P4RuntimeClient's StreamChannel does not busy-loop (99% CPU) on a
            # half-ready server — the startup race behind the corpus "no divergence" runs.
            channel = grpc.insecure_channel(addr)
            try:
                grpc.channel_ready_future(channel).result(timeout=0.5)
                return
            except grpc.FutureTimeoutError:
                time.sleep(delay)
            finally:
                channel.close()
        raise StepError(f"grpc service on {addr} did not become ready (see {self.log_path})")

    def _wait_for_thrift(self, attempts: int = 50, delay: float = 0.2) -> None:
        for _ in range(attempts):
            if self.proc and self.proc.poll() is not None:
                raise StepError(
                    f"simple_switch_grpc exited early; see {self.log_path}")
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                try:
                    s.connect((GRPC_HOST, THRIFT_PORT))
                    return
                except OSError:
                    time.sleep(delay)
        raise StepError(f"thrift port {THRIFT_PORT} did not come up (see {self.log_path})")

    def stop(self) -> None:
        if not self.proc:
            return
        try:
            subprocess.run(["sudo", "pkill", "-TERM", "-f", "simple_switch_grpc"],
                           check=False, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True, timeout=10)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                subprocess.run(["sudo", "pkill", "-KILL", "-f", "simple_switch_grpc"],
                               check=False, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               start_new_session=True, timeout=10)
                self.proc.wait(timeout=5)
        finally:
            _untrack_proc(self.proc)
            self.proc = None


@contextmanager
def bmv2_session(json_path: Path, log_path: Path, keep: bool):
    bm = Bmv2Process(json_path, log_path)
    bm.start()
    try:
        yield bm
    finally:
        if not keep:
            bm.stop()


# --------------------------------------------------------------------------- #
# txtpb parsing                                                               #
# --------------------------------------------------------------------------- #

@dataclass
class Phase:
    in_packet: bytes
    in_port: int
    exp_packet: Optional[bytes] = None
    exp_port: Optional[int] = None
    exp_mask: Optional[bytes] = None


@dataclass
class MulticastGroup:
    """A multicast group the harness installs before replay and removes after.

    Emitted only when the forwarding path is multicast (p4symbex models it as a single
    representative egress port, so ``replica_ports`` is typically one port).
    """
    mgid: int
    replica_ports: List[int] = field(default_factory=list)


@dataclass
class TamperCase:
    phases: List[Phase]
    entities: List[p4runtime_pb2.Entity]
    path: str
    affected_registers: List[dict] = field(default_factory=list)
    multicast_groups: List[MulticastGroup] = field(default_factory=list)


_BLOCK_RE = re.compile(r"^(\w+)\s*\{\s*$")
_PACKET_RE = re.compile(r'packet:\s*"((?:[^"\\]|\\.)*)"')
_MASK_RE = re.compile(r'packet_mask:\s*"((?:[^"\\]|\\.)*)"')
_PORT_RE = re.compile(r"port:\s*(\d+)")
_REG_NAME_RE = re.compile(r'register_name:\s*"([^"]*)"')
_REG_IDX_RE = re.compile(r'\bindex:\s*(\d+)')
_REG_VAL_RE = re.compile(r'attacker_value:\s*"((?:[^"\\]|\\.)*)"')
# match_kind is a proto ENUM: a bare identifier, not a quoted string.
_REG_KIND_RE = re.compile(r'\bmatch_kind:\s*([A-Za-z_][A-Za-z0-9_]*)')
_REG_MIN_RE = re.compile(r'min_value:\s*"((?:[^"\\]|\\.)*)"')


def _decode_escaped(s: str) -> bytes:
    return codecs.escape_decode(s.encode("latin-1"))[0]


def _iter_top_level_blocks(text: str):
    """Yield (header, body_text) for every top-level `header { ... }` block."""
    i = 0
    n = len(text)
    while i < n:
        # skip whitespace and comments
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        if text[i] == "#":
            while i < n and text[i] != "\n":
                i += 1
            continue
        # Find token (header)
        m = re.match(r"(\w+)\s*", text[i:])
        if not m:
            i += 1
            continue
        header = m.group(1)
        j = i + m.end()
        if j < n and text[j] == "{":
            depth = 1
            j += 1
            start = j
            while j < n and depth > 0:
                c = text[j]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                elif c == '"':
                    j += 1
                    while j < n:
                        if text[j] == "\\":
                            j += 2
                            continue
                        if text[j] == '"':
                            break
                        j += 1
                j += 1
            body = text[start:j - 1]
            yield header, body
            i = j
        else:
            # `name: ...` line — skip to next newline
            while i < n and text[i] != "\n":
                i += 1


def _parse_packet_block(body: str) -> Tuple[bytes, int, Optional[bytes]]:
    p = _PACKET_RE.search(body)
    if not p:
        raise ValueError("packet field missing in block")
    pkt = _decode_escaped(p.group(1))
    port_m = _PORT_RE.search(body)
    port = int(port_m.group(1)) if port_m else 0
    mask_m = _MASK_RE.search(body)
    mask = _decode_escaped(mask_m.group(1)) if mask_m else None
    return pkt, port, mask


def _entity_from_block(table_entry_body: str) -> p4runtime_pb2.Entity:
    te = p4runtime_pb2.TableEntry()
    text_format.Parse(table_entry_body, te, allow_unknown_field=True)
    ent = p4runtime_pb2.Entity()
    ent.table_entry.CopyFrom(te)
    return ent


def parse_tampering_txtpb(path: Path) -> TamperCase:
    text = path.read_text()
    phases: List[Phase] = []
    entities: List[p4runtime_pb2.Entity] = []
    affected_registers: List[dict] = []
    multicast_groups: List[MulticastGroup] = []
    current: Optional[Phase] = None

    for header, body in _iter_top_level_blocks(text):
        if header == "input_packet":
            if current is not None:
                phases.append(current)
            pkt, port, _ = _parse_packet_block(body)
            current = Phase(in_packet=pkt, in_port=port)
        elif header == "expected_output_packet":
            if current is None:
                raise ValueError(f"{path}: expected_output_packet without input_packet")
            pkt, port, mask = _parse_packet_block(body)
            current.exp_packet = pkt
            current.exp_port = port
            current.exp_mask = mask if mask is not None else b"\xFF" * len(pkt)
        elif header == "entities":
            inner = re.search(r"table_entry\s*\{", body)
            if not inner:
                continue
            depth = 0
            i = inner.end() - 1
            depth = 1
            i += 1
            start = i
            while i < len(body) and depth > 0:
                c = body[i]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                elif c == '"':
                    i += 1
                    while i < len(body):
                        if body[i] == "\\":
                            i += 2
                            continue
                        if body[i] == '"':
                            break
                        i += 1
                i += 1
            te_body = body[start:i - 1]
            entities.append(_entity_from_block(te_body))
        elif header == "affected_register":
            name_m = _REG_NAME_RE.search(body)
            idx_m = _REG_IDX_RE.search(body)
            val_m = _REG_VAL_RE.search(body)
            if name_m and idx_m and val_m:
                val_bytes = _decode_escaped(val_m.group(1))
                val_int = int.from_bytes(val_bytes, "big") if val_bytes else 0
                # Success criterion declared by the generator. Absent => exact, so files predating
                # the field keep their meaning. Without parsing these the walker would drop them
                # silently (its final branch ignores unknown headers) and exact-match forever.
                kind_m = _REG_KIND_RE.search(body)
                min_m = _REG_MIN_RE.search(body)
                min_bytes = _decode_escaped(min_m.group(1)) if min_m else b""
                affected_registers.append({
                    "name": name_m.group(1),
                    "index": int(idx_m.group(1)),
                    "attacker_value": val_int,
                    "match_kind": kind_m.group(1) if kind_m else "REGISTER_MATCH_EXACT",
                    "min_value": int.from_bytes(min_bytes, "big") if min_bytes else 0,
                })
        elif header == "multicast_group":
            mgid_m = re.search(r"mgid:\s*(\d+)", body)
            ports = [int(p) for p in re.findall(r"replica_port:\s*(\d+)", body)]
            if mgid_m:
                multicast_groups.append(
                    MulticastGroup(mgid=int(mgid_m.group(1)), replica_ports=ports))
        # ignore unknown headers (e.g. metadata/traces at top level are not blocks)

    if current is not None:
        phases.append(current)

    while len(phases) < 3:
        # Tolerate malformed files — pad with empty phases that will FAIL noisily.
        phases.append(Phase(in_packet=b"", in_port=0))
    return TamperCase(phases=phases[:3], entities=entities, path=str(path),
                      affected_registers=affected_registers,
                      multicast_groups=multicast_groups)


# --------------------------------------------------------------------------- #
# Control plane helpers                                                       #
# --------------------------------------------------------------------------- #

def load_p4info(client: P4RuntimeClient) -> p4info_pb2.P4Info:
    raw = client.get_p4info().SerializeToString()
    p4info = p4info_pb2.P4Info()
    p4info.ParseFromString(raw)
    return p4info


def clear_all_entries(client: P4RuntimeClient, p4info: p4info_pb2.P4Info) -> None:
    for table in p4info.tables:
        if getattr(table, "is_const_table", False):
            continue
        wildcard = p4runtime_pb2.Entity()
        wildcard.table_entry.table_id = table.preamble.id
        try:
            for response in client.read_one(wildcard):
                for ent in response.entities:
                    if ent.HasField("table_entry"):
                        # A default entry cannot be DELETEd -- P4Runtime has no notion of removing
                        # one, only of MODIFYing it -- so skip it rather than log a failure per
                        # table. A wildcard read does not normally return it, but do not rely on
                        # that now that the harness installs default entries itself.
                        if ent.table_entry.is_default_action:
                            continue
                        try:
                            # p4_client.py's delete dispatches on TableEntry, not Entity
                            client.delete(ent.table_entry)
                        except P4RuntimeWriteException as ex:
                            log.debug("delete failed (ok if const): %s", ex)
        except Exception as ex:  # pragma: no cover — defensive
            log.debug("read failed for table %s: %s", table.preamble.name, ex)


def _register_initial_values(cp_annotation: "Optional[Path]") -> "Dict[str, int]":
    """bare register name -> scalar initial value, from the --cp-annotation file.

    A cell that no packet has written does not read as zero when the P4 or the controller
    declares otherwise (SwitchV2P declares Register<key_pair_t,_>(SIZE, {1,0}) keys). Replay must
    therefore SET the state it assumes rather than trust the switch to be clean: a bare clear
    leaves every such register disagreeing with what symbex modelled. Struct initialisers are
    skipped here for the same reason symbex skips them - they need field-wise writes.
    """
    if not cp_annotation:
        return {}
    try:
        doc = json.loads(Path(cp_annotation).read_text())
    except (OSError, ValueError) as ex:
        # Only a missing/unreadable/malformed file is benign. A broad `except Exception` here
        # previously swallowed a NameError and made this silently return {} - the seeding looked
        # wired up but never ran.
        log.debug("cp-annotation initial values unavailable (%s): %s", cp_annotation, ex)
        return {}
    out = {}
    for so, rule in (doc.get("registers") or {}).items():
        iv = (rule or {}).get("initial_value") or {}
        val = iv.get("value")
        if isinstance(val, int):
            out[so.split(".")[-1]] = val
    return out


def clear_all_registers(
        client: P4RuntimeClient,
        p4info: p4info_pb2.P4Info,
        thrift_client: "Optional[Bmv2ThriftClient]" = None,
        initial_values: "Optional[Dict[str, int]]" = None) -> None:
    inits = initial_values or {}
    if thrift_client is not None:
        for reg in p4info.registers:
            bare = reg.preamble.name.split(".")[-1]
            try:
                thrift_client.clear_register(reg.preamble.name)
                if bare in inits:
                    # write_register seeds every cell; clear first so untouched cells match too
                    thrift_client.write_register_all(reg.preamble.name, inits[bare])
                    log.debug("seeded %s := %s", reg.preamble.name, inits[bare])
            except Exception as ex:
                log.debug("thrift register reset failed for %s: %s",
                          reg.preamble.name, ex)
        return
    for reg in p4info.registers:
        wildcard = p4runtime_pb2.Entity()
        wildcard.register_entry.register_id = reg.preamble.id
        try:
            for response in client.read_one(wildcard):
                for ent in response.entities:
                    if ent.HasField("register_entry"):
                        zero_entry = p4runtime_pb2.RegisterEntry()
                        zero_entry.CopyFrom(ent.register_entry)
                        zero_entry.ClearField("data")
                        try:
                            client.update(zero_entry)
                        except P4RuntimeWriteException as ex:
                            log.debug("register clear failed: %s", ex)
        except Exception as ex:
            log.debug("read failed for register %s: %s", reg.preamble.name, ex)


def _is_zero_register_data(data: p4data_pb2.P4Data) -> bool:
    kind = data.WhichOneof("data")
    if kind is None:
        return True
    if kind == "bitstring":
        return data.bitstring == bytes(len(data.bitstring))
    return data == p4data_pb2.P4Data()


def read_nonzero_registers(
        client: P4RuntimeClient,
        p4info: p4info_pb2.P4Info,
        thrift_client: "Optional[Bmv2ThriftClient]" = None) -> List[str]:
    nonzero: List[str] = []
    if thrift_client is not None:
        for reg in p4info.registers:
            try:
                vals = thrift_client.read_all(reg.preamble.name)
                for idx, val in enumerate(vals):
                    if val != 0:
                        nonzero.append(f"{reg.preamble.name}[{idx}]={val}")
            except Exception as ex:
                log.debug("thrift register read failed for %s: %s",
                          reg.preamble.name, ex)
        return nonzero
    for reg in p4info.registers:
        wildcard = p4runtime_pb2.Entity()
        wildcard.register_entry.register_id = reg.preamble.id
        try:
            for response in client.read_one(wildcard):
                for ent in response.entities:
                    if ent.HasField("register_entry"):
                        re = ent.register_entry
                        if not _is_zero_register_data(re.data):
                            idx = re.index.index if re.HasField("index") else "?"
                            nonzero.append(f"{reg.preamble.name}[{idx}]=non-zero")
        except Exception as ex:
            log.debug("read failed for register %s: %s", reg.preamble.name, ex)
    return nonzero

def get_registers(
        client: P4RuntimeClient,
        p4info: p4info_pb2.P4Info,
        thrift_client: "Optional[Bmv2ThriftClient]" = None) -> list:
    if thrift_client is not None:
        regs = []
        for reg in p4info.registers:
            try:
                regs.extend(thrift_client.read_all(reg.preamble.name))
            except Exception as ex:
                log.debug("thrift register read failed for %s: %s",
                          reg.preamble.name, ex)
        return regs
    regs = []
    for reg in p4info.registers:
        wildcard = p4runtime_pb2.Entity()
        wildcard.register_entry.register_id = reg.preamble.id
        try:
            for response in client.read_one(wildcard):
                for ent in response.entities:
                    if ent.HasField("register_entry"):
                        regs.append(ent.register_entry)
        except Exception as ex:
            log.debug("read failed for register %s: %s", reg.preamble.name, ex)
    return regs

def const_table_ids(p4info: p4info_pb2.P4Info) -> set:
    """Return the set of table ids that are `const entries` tables (immutable from the control
    plane). p4c sets P4Info Table.is_const_table for these; their entries are compiled in and
    cannot be installed/modified/deleted, so the harness must not write to them."""
    return {t.preamble.id for t in p4info.tables
            if getattr(t, "is_const_table", False)}


def _entity_match_sig(ent: p4runtime_pb2.Entity):
    """Hashable signature of a TableEntry's MATCH KEY (table_id + priority + match fields). The
    device holds one table state for all phases, so two entries with the same match key are the same
    physical entry; the second install would fail with ALREADY_EXISTS.

    `is_default_action` is part of the signature because a default entry carries NO match fields, so
    without it a table's default entry and a genuinely keyless match entry on that same table would
    produce an identical signature and one of them would be silently dropped."""
    te = ent.table_entry
    matches = tuple(sorted(m.SerializeToString() for m in te.match))
    return (te.table_id, te.priority, matches, te.is_default_action)


def install_entities(client: P4RuntimeClient, entities: List[p4runtime_pb2.Entity],
                     const_ids: set = frozenset()) -> None:
    # Tampering cases carry per-phase entity blocks (phase 1 and phase 2) that share the single
    # installed table state, so entries identical across phases (e.g. a sink-table entry the
    # cross-phase consistency pass pins in both phases) appear twice. Install each match key once.
    seen: set = set()
    for ent in entities:
        # A const-entries table is immutable; the control plane cannot install into it (its
        # entries are already present in the data plane). Skip rather than fail the Write.
        if ent.table_entry.table_id in const_ids:
            log.warn(f"skipping install into const-entries table id {ent.table_entry.table_id} "
                     f"(immutable)")
            continue
        sig = _entity_match_sig(ent)
        if sig in seen:
            continue  # identical cross-phase entry already installed
        seen.add(sig)
        try:
            # p4_client.py's insert/update dispatch on TableEntry, not Entity.
            #
            # A default entry always EXISTS -- every table has one, compiled in -- so P4Runtime
            # requires it to be written with MODIFY and rejects INSERT. This is the entry p4symbex
            # emits for a table whose default action the controller installs (a keyless table, e.g.
            # NdN's hashName_table or SwitchV2P's switch_config); without installing it the replay
            # runs p4c's compiled-in default, which is usually NoAction, and silently diverges from
            # the control plane the test was generated against.
            if ent.table_entry.is_default_action:
                client.update(ent.table_entry)
            else:
                client.insert(ent.table_entry)
        except P4RuntimeWriteException as ex:
            msg = str(ex)
            if "ALREADY_EXISTS" in msg:
                continue
            log.warn(f"Failed while installing {ent.table_entry}: {ex}")
            raise


def _build_multicast_entity(mc: "MulticastGroup") -> p4runtime_pb2.Entity:
    """Build a P4Runtime PacketReplicationEngine MulticastGroupEntry for `mc`.

    p4symbex models a multicast forward as a single representative egress port, so the
    group fans the packet out to `mc.replica_ports` (typically one port). The runtime
    multicast_group_id must equal the mcast_grp the packet carries.
    """
    ent = p4runtime_pb2.Entity()
    mge = ent.packet_replication_engine_entry.multicast_group_entry
    mge.multicast_group_id = mc.mgid
    rep_fields = {f.name for f in p4runtime_pb2.Replica.DESCRIPTOR.fields}
    for instance, port in enumerate(mc.replica_ports or [0], start=1):
        rep = mge.replicas.add()
        # P4Runtime Replica: bmv2 uses the uint32 `egress_port`; newer protos also expose a
        # bytes-typed `port`. Prefer egress_port (what simple_switch_grpc consumes).
        if "egress_port" in rep_fields:
            rep.egress_port = port
        else:
            rep.port = port.to_bytes(4, "big")
        rep.instance = instance
    return ent


def install_multicast_groups(client: P4RuntimeClient,
                             groups: List["MulticastGroup"]) -> None:
    for mc in groups:
        try:
            client.insert(_build_multicast_entity(mc))
        except Exception as ex:
            log.warn(f"Failed installing multicast group {mc.mgid}: {ex}")
            raise


def remove_multicast_groups(client: P4RuntimeClient,
                            groups: List["MulticastGroup"]) -> None:
    for mc in groups:
        try:
            client.delete(_build_multicast_entity(mc))
        except Exception as ex:
            log.debug("Failed removing multicast group %s: %s", mc.mgid, ex)


def read_entities(client: P4RuntimeClient, p4info: p4info_pb2.P4Info) -> List[p4runtime_pb2.Entity]:
    result = []
    for table in p4info.tables:
        if getattr(table, "is_const_table", False):
            continue
        wildcard = p4runtime_pb2.Entity()
        wildcard.table_entry.table_id = table.preamble.id
        try:
            for response in client.read_one(wildcard):
                for ent in response.entities:
                    if ent.HasField("table_entry"):
                        result.append(ent)
        except Exception as ex:
            log.debug("read failed for table %s: %s", table.preamble.name, ex)
    return result


# --------------------------------------------------------------------------- #
# Packet replay                                                               #
# --------------------------------------------------------------------------- #

def _masked_eq(actual: bytes, expected: bytes, mask: bytes) -> bool:
    n = len(expected)
    if len(actual) < n:
        return False
    a = bytes(x & m for x, m in zip(actual[:n], mask))
    e = bytes(x & m for x, m in zip(expected, mask))
    return a == e


def _pkt_diff(a: bytes, b: bytes, max_show: int = 12) -> str:
    """Human summary of where two equal-length output packets differ: byte offsets + values
    (legit -> attack). Falls back to a length note when the lengths differ."""
    if len(a) != len(b):
        return f"length {len(a)}B vs {len(b)}B"
    diffs = [(i, a[i], b[i]) for i in range(len(a)) if a[i] != b[i]]
    if not diffs:
        return "identical"
    head = ", ".join(f"@{i}:{x:02x}->{y:02x}" for i, x, y in diffs[:max_show])
    more = f" (+{len(diffs) - max_show} more)" if len(diffs) > max_show else ""
    return f"{len(diffs)} byte(s) [{head}{more}]"


def _sudo_prefix() -> List[str]:
    return [] if os.geteuid() == 0 else ["sudo", "-n"]


def _start_tcpdump(iface: str, pcap_path: Path) -> subprocess.Popen:
    """Start tcpdump on `iface` writing to `pcap_path` (sudo'd, with file
    world-readable so the user-mode script can read it back)."""
    pcap_path.parent.mkdir(parents=True, exist_ok=True)
    # remove first so tcpdump cleanly recreates with predictable perms
    pcap_path.unlink(missing_ok=True)
    cmd = [*_sudo_prefix(), "tcpdump", "-U", "--immediate-mode",
           "-i", iface, "-w", str(pcap_path)]
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    _track_proc(proc)
    return proc


def _stop_tcpdump(proc: subprocess.Popen, pcap_path: Path) -> None:
    if proc.poll() is None:
        # tcpdump is the sudo child; signal via sudo kill to traverse the
        # sudo wrapper, otherwise SIGINT to the sudo PID may not propagate.
        subprocess.run([*_sudo_prefix(), "pkill", "-INT", "-P", str(proc.pid)],
                       check=False, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       start_new_session=True)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            subprocess.run([*_sudo_prefix(), "pkill", "-KILL", "-P", str(proc.pid)],
                           check=False, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True)
            proc.wait(timeout=2)
    _untrack_proc(proc)
    if pcap_path.exists():
        subprocess.run([*_sudo_prefix(), "chmod", "0644", str(pcap_path)], check=False,
                        start_new_session=True)


def _read_pcap_bytes(pcap_path: Path) -> List[bytes]:
    if not pcap_path.exists() or pcap_path.stat().st_size == 0:
        return []
    try:
        return [bytes(p) for p in rdpcap(str(pcap_path))]
    except Exception as ex:
        log.debug("rdpcap %s: %s", pcap_path, ex)
        return []


_SUDO_SENDP_CODE = (
    "import sys; "
    "from scapy.all import sendp, Ether, conf; "
    "conf.verb = 0; "
    "iface = sys.argv[1]; "
    "data = sys.stdin.buffer.read(); "
    "sendp(Ether(data), iface=iface, verbose=False)"
)


def _sudo_sendp(packet: bytes, iface: str) -> None:
    cmd = [*_sudo_prefix(), sys.executable, "-c", _SUDO_SENDP_CODE, iface]
    subprocess.run(cmd, input=packet, check=True, timeout=10,
                   stdout=subprocess.DEVNULL, start_new_session=True,
                   stderr=subprocess.PIPE)


class PacketTester:
    def __init__(self, phase_timeout: float, capture_dir: Path, thrift_client=None,
                 strong_verify: bool = False):
        self.phase_timeout = phase_timeout
        self.capture_dir = capture_dir
        self.thrift_client = thrift_client
        # When True, the attacker value must be at the EXACT register index; a mismatch there
        # fails instead of accepting the value at some other index.
        self.strong_verify = strong_verify
        self.capture_dir.mkdir(parents=True, exist_ok=True)

    def _run_phase(self, phase: Phase, strict: bool, label: str) -> Tuple[bool, str]:
        in_iface = PORT_TO_HOST_IFACE.get(phase.in_port)
        if in_iface is None:
            return False, f"input port {phase.in_port} not mapped"

        sniff_ifaces = [i for i in ALL_HOST_IFACES if i != in_iface]
        pcaps = {i: self.capture_dir / f"{label}_{i}.pcap" for i in sniff_ifaces}
        # wipe any old pcaps so size==0 means "no packets"
        for p in pcaps.values():
            p.unlink(missing_ok=True)

        log.debug("phase %s: start tcpdump on %s", label, sniff_ifaces)
        dumps = {i: _start_tcpdump(i, pcaps[i]) for i in sniff_ifaces}
        try:
            # let tcpdump bind to interfaces before injecting
            time.sleep(0.4)
            log.debug("phase %s: sendp on %s (len=%d)", label, in_iface, len(phase.in_packet))
            _sudo_sendp(phase.in_packet, in_iface)
            time.sleep(self.phase_timeout)
        finally:
            for iface, p in dumps.items():
                _stop_tcpdump(p, pcaps[iface])

        captured = {i: _read_pcap_bytes(pcaps[i]) for i in sniff_ifaces}
        log.debug("phase %s: captured counts %s", label,
                  {k: len(v) for k, v in captured.items()})

        if not strict:
            return True, ""

        # translate captured-by-host-iface into captured-by-switch-port
        iface_to_port = {v: k for k, v in PORT_TO_HOST_IFACE.items()}
        by_port = {iface_to_port[i]: pkts for i, pkts in captured.items()}

        if phase.exp_packet is None:
            stray = {p: len(pkts) for p, pkts in by_port.items() if pkts}
            if stray:
                return False, f"expected no output, got {stray}"
            return True, ""

        exp_port = phase.exp_port
        on_exp = by_port.get(exp_port, [])
        elsewhere = {p: len(pkts) for p, pkts in by_port.items()
                     if p != exp_port and pkts}
        mask = phase.exp_mask or b"\xFF" * len(phase.exp_packet)
        match_idx = next(
            (idx for idx, pkt in enumerate(on_exp) if _masked_eq(pkt, phase.exp_packet, mask)),
            None,
        )
        if match_idx is None and not on_exp and not elsewhere:
            return False, f"expected packet on port {exp_port}, got none on any port"
        if match_idx is None:
            details = []
            if on_exp:
                first = on_exp[0]
                details.append(
                    f"port {exp_port} got {len(on_exp)} packet(s); first differs "
                    f"(len={len(first)} vs exp={len(phase.exp_packet)})"
                )
            if elsewhere:
                details.append(f"output on other ports {elsewhere}")
            return False, f"expected packet on port {exp_port} not seen; " + "; ".join(details)
        if elsewhere:
            return False, f"matched on port {exp_port} but also got output on {elsewhere}"
        if len(on_exp) > 1:
            return False, f"matched on port {exp_port} but {len(on_exp)} packets seen there (want 1)"
        return True, ""

    def _verify_attacker_registers(self, affected_registers: List[dict]) -> Tuple[bool, str]:
        """Verify that Phase 2 wrote the attacker-chosen value to the register via Thrift."""
        if self.thrift_client is None or not self.thrift_client.is_alive():
            return True, ""
        for reg in affected_registers:
            try:
                vals = self.thrift_client.read_all(reg["name"])
                idx, expected = reg["index"], reg["attacker_value"]
                # Success criterion declared by the generator (see AffectedRegister in
                # p4symbex.proto). AT_LEAST is used for an ACCUMULATING register, whose exact value
                # is not observable: the harness stops driving it once it reaches the target, and a
                # lost packet changes where that lands. Crossing min_value -- the value at which the
                # sink flips -- is the real success condition. Absent => exact, as before.
                at_least = (reg.get("match_kind") == "REGISTER_MATCH_AT_LEAST"
                            and reg.get("min_value", 0) > 0)
                bound = reg.get("min_value", 0)
                ok = (lambda v: v is not None and (v >= bound if at_least else v == expected))
                want = f">= {hex(bound)}" if at_least else f"== {hex(expected)}"
                actual = vals[idx] if idx < len(vals) else None
                if not ok(actual):
                    # Strong: a mismatch at the exact index is a hard failure. Weak: a non-zero
                    # mismatch fails, but a zero may mean the value landed at another index.
                    if actual != 0 or self.strong_verify:
                        return False, (
                            f"register {reg['name']}[{idx}]="
                            f"{'None' if actual is None else hex(actual)}, want {want}"
                        )
                    for i, val in enumerate(vals):
                        if i != idx and ok(val):
                            log.warn("register %s[%d] satisfies %s (expected index %d)",
                                     reg["name"], i, want, idx)
                            return True, ""
                    return False, (
                        f"register {reg['name']}[{idx}]={hex(actual) if actual is not None else 'None'}, "
                        f"want {want}"
                    )
            except Exception as ex:
                log.debug("register verify failed for %s: %s", reg["name"], ex)
        return True, ""

    def _observe_phase(self, phase: Phase, reference: Phase, label: str) -> Tuple[bool, str]:
        """Send Phase 3 packet and compare output to Phase 1 reference.

        Returns (True, reason) if output differs from Phase 1 (tampering effective),
        or (False, reason) if output matches Phase 1 (false positive).
        """
        in_iface = PORT_TO_HOST_IFACE.get(phase.in_port)
        if in_iface is None:
            return False, f"input port {phase.in_port} not mapped"
        sniff_ifaces = [i for i in ALL_HOST_IFACES if i != in_iface]
        pcaps = {i: self.capture_dir / f"{label}_{i}.pcap" for i in sniff_ifaces}
        for p in pcaps.values():
            p.unlink(missing_ok=True)
        dumps = {i: _start_tcpdump(i, pcaps[i]) for i in sniff_ifaces}
        try:
            time.sleep(0.4)
            log.debug("phase %s: sendp on %s (len=%d)", label, in_iface, len(phase.in_packet))
            _sudo_sendp(phase.in_packet, in_iface)
            time.sleep(self.phase_timeout)
        finally:
            for iface, p in dumps.items():
                _stop_tcpdump(p, pcaps[iface])
        captured = {i: _read_pcap_bytes(pcaps[i]) for i in sniff_ifaces}
        iface_to_port = {v: k for k, v in PORT_TO_HOST_IFACE.items()}
        by_port = {iface_to_port[i]: pkts for i, pkts in captured.items()}
        observed_port = next((p for p, pkts in by_port.items() if pkts), None)
        observed_pkt = by_port[observed_port][0] if observed_port is not None else None
        ref_port, ref_pkt = reference.exp_port, reference.exp_packet
        ref_mask = reference.exp_mask or (b"\xFF" * len(ref_pkt) if ref_pkt else b"")
        if observed_port != ref_port:
            return True, f"output port changed: {ref_port} → {observed_port}"
        if ref_pkt is not None and observed_pkt is not None:
            if not _masked_eq(observed_pkt, ref_pkt, ref_mask):
                return True, f"packet bytes differ on port {observed_port}"
            return False, "no deviation from phase1 (false positive)"
        if (observed_pkt is None) != (ref_pkt is None):
            return True, f"drop state changed (phase1 forwarded={ref_pkt is not None})"
        return False, "no deviation from phase1 (false positive)"

    def _send_capture(self, phase: Phase, label: str) -> Optional[Tuple[int, bytes]]:
        """Send one phase packet and capture the switch's output as (port, first_packet_bytes),
        or None if dropped. No verification — used by the differential replay."""
        in_iface = PORT_TO_HOST_IFACE.get(phase.in_port)
        if in_iface is None:
            return None
        sniff_ifaces = [i for i in ALL_HOST_IFACES if i != in_iface]
        pcaps = {i: self.capture_dir / f"{label}_{i}.pcap" for i in sniff_ifaces}
        for p in pcaps.values():
            p.unlink(missing_ok=True)
        dumps = {i: _start_tcpdump(i, pcaps[i]) for i in sniff_ifaces}
        try:
            time.sleep(0.4)
            _sudo_sendp(phase.in_packet, in_iface)
            time.sleep(self.phase_timeout)
        finally:
            for iface, p in dumps.items():
                _stop_tcpdump(p, pcaps[iface])
        captured = {i: _read_pcap_bytes(pcaps[i]) for i in sniff_ifaces}
        iface_to_port = {v: k for k, v in PORT_TO_HOST_IFACE.items()}
        by_port = {iface_to_port[i]: pkts for i, pkts in captured.items()}
        port = next((p for p, pkts in by_port.items() if pkts), None)
        if port is None:
            return None
        return port, by_port[port][0]

    def replay(self, case: TamperCase, phase_indices: List[int], label: str,
               verify_regs_after: Optional[int] = None
               ) -> Tuple[Optional[Tuple[int, bytes]], str]:
        """Send the given phases in order (entities/groups installed by the caller after a reset);
        return (observed output of the LAST phase, reg_diag). When `verify_regs_after` is the index
        of the phase that writes the attacker register (Phase 2 = 1), check the register right
        after that phase (before Phase 3 overwrites it) and return a diagnostic string."""
        last: Optional[Tuple[int, bytes]] = None
        reg_diag = ""
        for i in phase_indices:
            last = self._send_capture(case.phases[i], f"{label}_p{i + 1}")
            if i == verify_regs_after and case.affected_registers:
                try:
                    ok, why = self._verify_attacker_registers(case.affected_registers)
                except Exception as ex:  # noqa: BLE001
                    ok, why = False, str(ex)
                reg_diag = "written" if ok else f"NOT-written ({why})"
        return last, reg_diag

    def compare_runs(self, legit: Optional[Tuple[int, bytes]],
                     attack: Optional[Tuple[int, bytes]]) -> Tuple[bool, str]:
        """Differential oracle: legit = Phase-3 output of (P1 -> P3); attack = (P1 -> P2 -> P3).
        Both share Phase 1, so any difference is the attacker's Phase-2 write. Returns
        (vulnerable, reason)."""
        if legit == attack:
            return False, ("no divergence: Phase-2 write has no observable effect on the replayed "
                           "Phase-3 output (benign / masked corruption)")

        def desc(o: Optional[Tuple[int, bytes]]) -> str:
            return "drop" if o is None else f"port {o[0]}, {len(o[1])}B"

        if (legit is None) != (attack is None):
            kind = "drop-state changed"
        elif legit is not None and attack is not None and legit[0] != attack[0]:
            kind = "output port changed"
        else:
            kind = f"packet bytes differ — {_pkt_diff(legit[1], attack[1])}"
        return True, (f"VULNERABLE: attacker's Phase-2 write changes the replayed Phase-3 output "
                      f"({kind}: legit={desc(legit)} vs attack={desc(attack)})")

    def run(self, case: TamperCase, label: str) -> Tuple[bool, str]:
        # Phase 1: strict — confirm read path and baseline output
        ok, reason = self._run_phase(case.phases[0], strict=True, label=f"{label}_p1")
        if not ok:
            return False, f"phase1: {reason}"
        # Phase 2: lenient — write attacker value to register
        self._run_phase(case.phases[1], strict=False, label=f"{label}_p2")
        # After Phase 2: verify register was written with attacker-chosen value
        if case.affected_registers:
            ok, reason = self._verify_attacker_registers(case.affected_registers)
            if not ok:
                return False, f"phase2_reg_check: {reason}"
        # Phase 3: dynamic — replay Phase 1's packet; detect deviation from Phase 1 output.
        # Deviation = tampering effective (true positive).
        # No deviation = tampering had no observable effect (false positive).
        deviated, reason = self._observe_phase(
            case.phases[2], reference=case.phases[0], label=f"{label}_p3"
        )
        if not deviated:
            return False, f"phase3_no_deviation: {reason}"
        return True, ""


# --------------------------------------------------------------------------- #
# Per-stage drivers                                                           #
# --------------------------------------------------------------------------- #

def do_build(spec: TargetSpec, args) -> Tuple[Path, Path]:
    out_root = Path(args.output_root).expanduser()
    build_dir = out_root / spec.name / "build"
    return run_p4c(spec.name, spec.p4_file, spec.extra_args, build_dir, spec.p4_version)


def do_p4symbex(spec: TargetSpec, args) -> List[Path]:
    out_root = Path(args.output_root).expanduser()
    protobuf_dir = out_root / spec.name / "protobuf"
    return run_p4symbex(
        spec.name, spec.p4_file, protobuf_dir,
        max_tests=args.max_tests,
        tamper_value=args.tamper_value,
        skip=args.skip_p4symbex,
        p4_version=spec.p4_version,
        extra_args=spec.extra_args,
    )


def do_testing(spec: TargetSpec, json_path: Path, p4info_path: Path,
               txtpb_files: List[Path], args, csv_writer, progress_cb) -> Tuple[int, int, int]:
    """Run BMv2 + all protobufs for one target.

    Returns (ok_count, fail_count, err_count) at protobuf granularity:
      - ok_count: protobufs whose replay matched expectations
      - fail_count: protobufs where verification mismatched (a real divergence)
      - err_count: protobufs where the test infrastructure itself errored
                   (BMv2 didn't start, entity install failed, replay raised, ...)
    """
    target_root = Path(args.output_root).expanduser() / spec.name
    capture_dir = target_root / "pcap"
    # Replay must establish the state symbex assumed, not trust the switch to be clean; resolved
    # the same way builders._cp_annotation_for does so no caller needs a new parameter.
    _ann_root = getattr(args, "cp_annotation_root", None)
    _ann_path = (Path(_ann_root).expanduser() / f"{spec.name}.json") if _ann_root else None
    _reg_inits = _register_initial_values(_ann_path if _ann_path and _ann_path.exists() else None)
    if _reg_inits:
        log.info("[%s] seeding %d register(s) to declared initial values before each case",
                 spec.name, len(_reg_inits))
    ok_count = 0
    fail_count = 0
    err_count = 0
    skip_count = 0

    def emit(protobuf_file, result, reason):
        nonlocal ok_count, fail_count, err_count, skip_count
        csv_writer(spec.name, protobuf_file, result, reason)
        if result == "OK":
            ok_count += 1
        elif result == "FAIL":
            fail_count += 1
        elif result == "SKIP":
            skip_count += 1
        else:
            err_count += 1

    # Clean-state strategy between cases (see --reset-mode): 'registers' clears tables + zeroes
    # register cells via the control plane; 'restart' bounces simple_switch_grpc for a pristine
    # state (no register-clear message burst on wide arrays).
    reset_mode = getattr(args, "reset_mode", "registers")
    restart_mode = reset_mode == "restart"

    try:
        with bmv2_session(json_path, target_root / "bmv2.log", args.keep_bmv2) as bm:
            def _connect():
                """(Re)connect P4Runtime + Thrift to the running simple_switch_grpc and push the
                pipeline. Used at startup and after each restart in --reset-mode restart."""
                client = P4RuntimeClient(
                    device_id=DEVICE_ID,
                    grpc_address=f"{GRPC_HOST}:{GRPC_PORT}",
                    election_id=ELECTION_ID,
                )
                thrift_client = Bmv2ThriftClient(GRPC_HOST, THRIFT_PORT)
                if not thrift_client.is_alive():
                    log.warning("[%s] Thrift client connected but probe failed on %s:%d",
                                spec.name, GRPC_HOST, THRIFT_PORT)
                client.set_fwd_pipe_config(str(p4info_path), str(json_path))
                tester = PacketTester(args.phase_timeout, capture_dir, thrift_client=thrift_client,
                                      strong_verify=getattr(args, "strong_verify", False))
                return client, thrift_client, tester

            log.info("[%s] connecting P4Runtime client", spec.name)
            client, thrift_client, tester = _connect()
            try:
                # p4info / const_ids are program-defined, so they survive restarts unchanged.
                p4info = load_p4info(client)
                const_ids = const_table_ids(p4info)
                if const_ids:
                    log.info("[%s] %d const-entries table(s) detected (immutable; not installed/cleared)",
                             spec.name, len(const_ids))
                total = len(txtpb_files)
                all_regs = get_registers(client, p4info, thrift_client=thrift_client)
                if len(all_regs) == 0:
                    log.error("[%s] no registers found in P4Info; is this really a v1model target?", spec.name)
                # Reset to a pristine, identical state (both differential runs must start the
                # same), then install this case's entities + multicast group.
                def _reset_state():
                    nonlocal client, thrift_client, tester
                    if restart_mode:
                        try:
                            client.tear_down()
                        except Exception:
                            pass
                        try:
                            thrift_client.close()
                        except Exception:
                            pass
                        bm.stop()
                        bm.start()
                        client, thrift_client, tester = _connect()
                    else:
                        clear_all_entries(client, p4info)
                        clear_all_registers(client, p4info, thrift_client=thrift_client,
                                            initial_values=_reg_inits)

                def _install_for_run(case):
                    install_entities(client, case.entities, const_ids)
                    install_multicast_groups(client, case.multicast_groups)

                for idx, tx in enumerate(txtpb_files, 1):
                    if SHUTDOWN.is_set():
                        break
                    progress_cb(idx - 1, total, tx.name)  # report "starting"
                    try:
                        case = parse_tampering_txtpb(tx)
                        # Skip cases whose any phase packet is below the Ethernet minimum:
                        # scapy's Ether() cannot dissect a sub-14-byte frame.
                        too_short = next((len(ph.in_packet) for ph in case.phases
                                          if len(ph.in_packet) < GLOBAL_MIN_PACKET_SIZE), None)
                        if too_short is not None:
                            emit(tx.name, "SKIP",
                                 f"packet {too_short}B < min {GLOBAL_MIN_PACKET_SIZE}B")
                            progress_cb(idx, total, tx.name)
                            continue
                        # Differential oracle: legit (Phase 1 -> Phase 3) vs attack (Phase 1 ->
                        # Phase 2 -> Phase 3); VULNERABLE iff the Phase-3 outputs differ.
                        _reset_state()
                        _install_for_run(case)
                        legit, _ = tester.replay(case, [0, 2], label=f"{tx.stem}_legit")
                        remove_multicast_groups(client, case.multicast_groups)
                        _reset_state()
                        _install_for_run(case)
                        attack, reg_diag = tester.replay(case, [0, 1, 2],
                                                         label=f"{tx.stem}_attack",
                                                         verify_regs_after=1)
                        remove_multicast_groups(client, case.multicast_groups)
                        ok, reason = tester.compare_runs(legit, attack)
                        if reg_diag:
                            reason = f"{reason} [Phase-2 reg: {reg_diag}]"
                    except P4RuntimeWriteException as ex:
                        emit(tx.name, "ERROR", f"install: {ex}")
                        progress_cb(idx, total, tx.name)
                        continue
                    except Exception as ex:
                        emit(tx.name, "ERROR", f"replay: {ex}")
                        progress_cb(idx, total, tx.name)
                        continue
                    emit(tx.name, "OK" if ok else "FAIL", reason)
                    progress_cb(idx, total, tx.name)
            finally:
                # client/thrift_client may have been swapped by a restart; tear down whatever
                # the latest references point to, tolerating an already-closed handle.
                try:
                    client.tear_down()
                except Exception:
                    pass
                try:
                    thrift_client.close()
                except Exception:
                    pass
    except StepError as ex:
        log.error("[%s] BMv2/grpc failure: %s", spec.name, ex)
        emit("", "ERROR", f"bmv2: {ex}")

    return ok_count, fail_count, err_count


# --------------------------------------------------------------------------- #
# Live status + pipeline                                                      #
# --------------------------------------------------------------------------- #

class Status:
    """Thread-safe live state for the three-line TUI.

    Per-stage counters are at TARGET granularity:
      - *_ok: targets that completed this step successfully
      - *_err: targets where this step itself failed (p4c crash, p4symbex
               crash / produced no .txtpb, BMv2 startup / RPC error, all
               protobufs ERRORed). Each target contributes to the *_err of at
               most one step (the first that fails).
    """

    def __init__(self, total: int):
        self.lock = threading.Lock()
        self.total = total
        # phase counters (target granularity)
        self.gen_done = 0
        self.gen_ok = 0
        self.gen_err = 0
        self.gen_active: List[str] = []
        self.compile_done = 0
        self.compile_ok = 0
        self.compile_err = 0
        self.compile_active: List[str] = []
        self.test_done = 0
        self.test_ok = 0
        self.test_fail = 0
        self.test_err = 0
        # in-flight test details
        self.test_target: str = ""
        self.test_tx_total: int = 0
        self.test_tx_idx: int = 0
        self.test_tx_name: str = ""

    def _fmt_active(self, names: List[str]) -> str:
        if not names:
            return "-"
        if len(names) <= 2:
            return ", ".join(names)
        return f"{names[0]}, {names[1]} (+{len(names) - 2} more)"

    def render(self, target: str = "switch") -> Tuple[str, str, str]:
        with self.lock:
            t = self.total
            pct = lambda n: (100.0 * n / t) if t else 0.0
            l1 = (f"(1) gen (p4symbex): {self.gen_ok}/{t} "
                  f"({pct(self.gen_ok):.0f}% success, {self.gen_err} errors)"
                  f"  running {self._fmt_active(self.gen_active)}")
            l2 = (f"(2) compile (p4c):  {self.compile_ok}/{t} "
                  f"({pct(self.compile_ok):.0f}% success, {self.compile_err} errors)"
                  f"  building {self._fmt_active(self.compile_active)}")
            tx_info = "-"
            if self.test_target:
                if self.test_tx_total:
                    tx_pct = 100.0 * self.test_tx_idx / self.test_tx_total
                    tx_info = (f"{self.test_target} "
                               f"[{self.test_tx_name} "
                               f"({self.test_tx_idx}/{self.test_tx_total}: {tx_pct:.0f}%)]")
                else:
                    tx_info = self.test_target
            l3 = (f"(3) {target} testing: {self.test_ok}/{t} "
                  f"({pct(self.test_ok):.0f}% success, "
                  f"{self.test_fail} fail, {self.test_err} errors)"
                  f"  testing {tx_info}")
            return l1, l2, l3


class Renderer:
    """Repaints three TUI lines in place using ANSI escape codes."""

    def __init__(self, status: Status, enabled: bool, period: float = 0.2,
                 target: str = "switch"):
        self.status = status
        self.enabled = enabled and sys.stdout.isatty()
        self.period = period
        self.target = target
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def _draw(self, first: bool) -> None:
        l1, l2, l3 = self.status.render(target=self.target)
        # Truncate to terminal width so no line wraps onto a second row.
        # Wrapping would add extra terminal rows, making \033[3A land in the
        # wrong place on the next redraw (causes the "new line" symptom with
        # long Tofino program names).
        try:
            cols = os.get_terminal_size().columns - 1
            l1, l2, l3 = l1[:cols], l2[:cols], l3[:cols]
        except OSError:
            pass
        out = sys.stdout
        if first:
            out.write(l1 + "\n" + l2 + "\n" + l3 + "\n")
        else:
            # move up 3 lines, then rewrite each clearing to end-of-line
            out.write("\033[3A\r\033[K" + l1 + "\n\033[K" + l2 + "\n\033[K" + l3 + "\n")
        out.flush()

    def _loop(self) -> None:
        try:
            self._draw(first=True)
            while not self._stop.wait(self.period):
                self._draw(first=False)
            self._draw(first=False)  # final
        finally:
            sys.stdout.write("\033[?25h")  # show cursor
            sys.stdout.flush()

    def start(self) -> None:
        if not self.enabled:
            return
        sys.stdout.write("\033[?25l")  # hide cursor
        self._thread = threading.Thread(target=self._loop, name="tui", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        if self._thread:
            self._stop.set()
            self._thread.join(timeout=2)


class Pipeline:
    """Stage runner: gen pool (p4symbex) → compile pool (p4c) → single test worker.

    Stage order:
      gen:  p4symbex in parallel (--jobs workers) → produces .txtpb files
      run:  p4c/cmake in parallel (--jobs workers), then packet testing single-process
      all:  gen, then run

    Testing is always single-process because tofino_model / simple_switch_grpc
    are system-wide binaries sharing veth interfaces.
    """

    def __init__(self, targets: List[TargetSpec], args, csv_writer_fn, status: Status, builder=None):
        self.targets = targets
        self.args = args
        self.csv_writer = csv_writer_fn
        self.status = status
        self.builder = builder

    def _csv(self, *row) -> None:
        self.csv_writer(*row)

    # ------------------------------------------------------------------ gen --

    def _gen_task(self, spec: TargetSpec, out: Path) -> None:
        if SHUTDOWN.is_set():
            return
        with self.status.lock:
            self.status.gen_active.append(spec.name)
        try:
            txtpb_files = self.builder.p4symbex(spec, out, self.args)
            ok = bool(txtpb_files)
            with self.status.lock:
                self.status.gen_done += 1
                if ok:
                    self.status.gen_ok += 1
                else:
                    self.status.gen_err += 1
                if spec.name in self.status.gen_active:
                    self.status.gen_active.remove(spec.name)
            if not ok:
                self._csv(spec.name, "", "ERROR", "gen produced no .txtpb files")
        except StepError as ex:
            with self.status.lock:
                self.status.gen_done += 1
                self.status.gen_err += 1
                if spec.name in self.status.gen_active:
                    self.status.gen_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"gen: {ex}")
        except Exception as ex:
            with self.status.lock:
                self.status.gen_done += 1
                self.status.gen_err += 1
                if spec.name in self.status.gen_active:
                    self.status.gen_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"gen-unhandled: {ex}")

    def _run_gen_stage(self, jobs: int, out: Path) -> None:
        with self.status.lock:
            # compile and test are not part of this stage
            self.status.compile_done = len(self.targets)
            self.status.compile_ok = len(self.targets)
            self.status.test_done = len(self.targets)
        pool = ThreadPoolExecutor(max_workers=jobs, thread_name_prefix="gen")
        try:
            # Cheap-first: submit low-timeout programs before expensive ones so the many default
            # (1800s) programs drain first and the few high-timeout ones run last, instead of a
            # handful of expensive programs seizing all job slots and starving the tail of the cell.
            for spec in sorted(self.targets, key=lambda s: s.timeout):
                if SHUTDOWN.is_set():
                    break
                pool.submit(self._gen_task, spec, out)
            pool.shutdown(wait=True)
        except KeyboardInterrupt:
            try:
                pool.shutdown(wait=False, cancel_futures=True)
            except TypeError:
                pool.shutdown(wait=False)

    # --------------------------------------------------------------- compile --

    def _compile_task(self, spec: TargetSpec, out: Path) -> None:
        if SHUTDOWN.is_set():
            return
        with self.status.lock:
            self.status.compile_active.append(spec.name)
        try:
            json_path, p4info_path = self.builder.build(spec, out, self.args)
            with self.status.lock:
                self.status.compile_done += 1
                self.status.compile_ok += 1
                if spec.name in self.status.compile_active:
                    self.status.compile_active.remove(spec.name)
            # Find pre-existing .txtpb files generated by gen stage.
            try:
                _, _, txtpb_files = self.builder.find_artifacts(spec, out, self.args)
            except StepError as ex:
                with self.status.lock:
                    self.status.test_done += 1
                    self.status.test_err += 1
                self._csv(spec.name, "", "ERROR", f"no .txtpb (run gen first): {ex}")
                return
            self._test_q.put((spec, json_path, p4info_path, txtpb_files))
        except StepError as ex:
            with self.status.lock:
                self.status.compile_done += 1
                self.status.compile_err += 1
                self.status.test_done += 1
                if spec.name in self.status.compile_active:
                    self.status.compile_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"compile: {ex}")
        except Exception as ex:
            with self.status.lock:
                self.status.compile_done += 1
                self.status.compile_err += 1
                self.status.test_done += 1
                if spec.name in self.status.compile_active:
                    self.status.compile_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"compile-unhandled: {ex}")

    # ----------------------------------------------------------------- test --

    def _test_worker(self) -> None:
        while True:
            item = self._test_q.get()
            if item is None:
                return
            if SHUTDOWN.is_set():
                continue
            spec, json_path, p4info_path, txtpb_files = item

            def progress_cb(idx: int, total: int, name: str, _s=spec) -> None:
                with self.status.lock:
                    self.status.test_target = _s.name
                    self.status.test_tx_total = total
                    self.status.test_tx_idx = idx
                    self.status.test_tx_name = name

            try:
                ok_count, fail_count, err_count = self.builder.test(
                    spec, (json_path, p4info_path), txtpb_files,
                    self.args, self.csv_writer, progress_cb,
                )
            except Exception as ex:
                ok_count, fail_count, err_count = 0, 0, 1
                import traceback as _tb
                log.error("test-unhandled traceback:\n%s", _tb.format_exc())
                self._csv(spec.name, "", "ERROR", f"test-unhandled: {ex}")

            with self.status.lock:
                self.status.test_done += 1
                if err_count > 0:
                    self.status.test_err += 1
                elif fail_count == 0 and ok_count > 0:
                    self.status.test_ok += 1
                else:
                    self.status.test_fail += 1
                self.status.test_target = ""
                self.status.test_tx_total = 0
                self.status.test_tx_idx = 0
                self.status.test_tx_name = ""

    def _run_compile_and_test(self, jobs: int, out: Path) -> None:
        """Compile all targets in parallel, then test them one at a time."""
        with self.status.lock:
            # gen is not part of this stage
            self.status.gen_done = len(self.targets)
            self.status.gen_ok = len(self.targets)

        test_thread = threading.Thread(target=self._test_worker, name="test", daemon=True)
        test_thread.start()

        bpool = ThreadPoolExecutor(max_workers=jobs, thread_name_prefix="compile")
        try:
            for spec in self.targets:
                if SHUTDOWN.is_set():
                    break
                bpool.submit(self._compile_task, spec, out)
            bpool.shutdown(wait=True)
        except KeyboardInterrupt:
            try:
                bpool.shutdown(wait=False, cancel_futures=True)
            except TypeError:
                bpool.shutdown(wait=False)
        finally:
            self._test_q.put(None)
            test_thread.join()

    # --------------------------------------------------------------- stages --

    def run(self) -> None:
        self._test_q: "queue.Queue" = queue.Queue()
        jobs = max(1, self.args.jobs)
        out = Path(self.args.output_root).expanduser()

        if self.args.stage == "gen":
            self._run_gen_stage(jobs, out)
            return

        if self.args.stage == "run":
            self._run_compile_and_test(jobs, out)
            return

        # "all": gen then run
        self._run_gen_stage(jobs, out)
        # Reset the skipped-stage markers set by _run_gen_stage before running compile+test.
        with self.status.lock:
            self.status.compile_done = 0
            self.status.compile_ok = 0
            self.status.test_done = 0
        if not SHUTDOWN.is_set():
            self._run_compile_and_test(jobs, out)


# --------------------------------------------------------------------------- #
# Main                                                                        #
# --------------------------------------------------------------------------- #

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("bmv2", "tofino"), default="bmv2",
                        help="switch target (bmv2 → simple_switch_grpc / P4Runtime; "
                             "tofino → tofino_model / BF-Runtime)")
    parser.add_argument("--stage", choices=("gen", "run", "all"), default=None,
                        help="gen=p4symbex only; run=compile+test; all=both "
                             "(bmv2 default=all, tofino default=gen)")
    parser.add_argument("--arch", default=None,
                        help="P4 architecture (v1model for bmv2; tna or t2na for tofino). "
                             "Defaults to v1model / tna based on --target.")
    parser.add_argument("--p4-file", default=None,
                        help="run the pipeline against this single .p4 file, "
                             "ignoring --program-list. Useful for ad-hoc smoke tests.")
    parser.add_argument("--target-name", default=None,
                        help="name used for output dirs when --p4-file is set "
                             "(default: stem of --p4-file)")
    parser.add_argument("--p4c-bin", default=None,
                        help="override p4c binary path (defaults to ~/Workspace/p4c/build/p4c)")
    parser.add_argument("--p4symbex-bin", default=None,
                        help="override p4symbex binary path "
                             "(defaults to ~/Workspace/p4c/build/p4symbex)")
    parser.add_argument("--state-dep-cache-root", default=None,
                        help="directory of pre-computed SOChain caches (built by "
                             "nightly/cache_chains.py). When set, p4symbex loads "
                             "<root>/<target>/<arch>/<name>.chains via --state-dep-cache instead of "
                             "re-running the IFDS analysis (hard-errors if a cache is missing/stale).")
    parser.add_argument("--cp-annotation-root",
                        default=str(HOME / "Workspace-remote/p4symbex_nightly/annotations"),
                        help="directory of external control-plane / port annotations. For each "
                             "program, <root>/<name>.json is passed to p4symbex via --cp-annotation. "
                             "Optional data: a program with no file runs unannotated, unchanged.")
    parser.add_argument("--program-list",
                        default=str(HOME / "Workspace-remote/top_tier_repo/program_list.txt"))
    parser.add_argument("--output-root",
                        default=str(HOME / "Workspace-remote/top_tier_repo/output"))
    parser.add_argument("--csv", default=None,
                        help="CSV path (default: <output-root>/tampering_results.csv)")
    parser.add_argument("--filter", default=None,
                        help="substring match on target name; runs only matching rows")
    parser.add_argument("--skip-p4symbex", action="store_true",
                        help="reuse existing .txtpb files instead of regenerating")
    parser.add_argument("--symbex-timeout-scale", type=float, default=1.0, metavar="F",
                        help="multiply every program's symbex_timeout= budget by F. The program "
                             "lists are the record of what the nightly does; this is the run-time "
                             "override for a retry, so the lists never need editing.")
    parser.add_argument("--symbex-timeout-min", type=int, default=0, metavar="SECS",
                        help="raise any program budgeted below SECS up to SECS. A floor, not a "
                             "scale: use it to give the cheap tiers enough room to finish while "
                             "leaving the expensive tier (which may be intractable anyway) alone. "
                             "Applied after --symbex-timeout-scale.")
    parser.add_argument("--keep-bmv2", action="store_true",
                        help="leave BMv2 running between targets (debug)")
    parser.add_argument("--sde-cmake-build", action="store_true",
                        help="tofino: use cmake+make install (SDE p4studio) for compile step")
    parser.add_argument("--sde-manage-procs", action="store_true",
                        help="tofino: auto-start/stop run_tofino_model.sh + run_switchd.sh")
    parser.add_argument("--force-compile", action="store_true",
                        help="tofino: recompile even when artifacts already exist. Both compile "
                             "paths skip when they find prior output — run_p4c when build/ holds a "
                             "bfrt.json or context.json, and the cmake path when "
                             "$SDE_INSTALL/share/tofinopd/<name>/ holds a .bin. Those checks look "
                             "only for PRESENCE, never at whether the .p4 is newer, so editing a "
                             "program silently replays the stale binary. Use this after changing a "
                             "P4 source (or its includes, which nothing tracks at all).")
    parser.add_argument("--shared-traversal", choices=("NONE", "PHASE1", "PHASE1_PHASE2"),
                        default=None,
                        help="forwarded to p4symbex --shared-traversal. PHASE1_PHASE2 adds the "
                             "global Phase-2 write-path prefilter; NONE is a measurement baseline. "
                             "Omitted -> p4symbex default (PHASE1).")
    parser.add_argument("--chain-impact-order", action="store_true",
                        help="forwarded to p4symbex --chain-impact-order: process chains whose "
                             "sink reaches an enforcement primitive first. Ordering only, so it "
                             "changes which chains a timed-out run reaches, never the results.")
    parser.add_argument("--reset-mode", choices=("registers", "restart"),
                        default="registers",
                        help="how to get a clean state between txtpb cases (both targets). "
                             "'registers' (default) clears tables + zeroes register cells via "
                             "the control plane; 'restart' instead restarts the switch process "
                             "(bmv2: simple_switch_grpc; tofino: tofino_model + bf_switchd) for a "
                             "pristine state, avoiding the register-clear message burst on wide "
                             "arrays. For tofino, 'restart' requires --sde-manage-procs.")
    parser.add_argument("--phase-timeout", type=float, default=2.0)
    parser.add_argument("--strong-verify", action="store_true",
                        help="require the attacker value at the EXACT register index (both "
                             "targets). Default (weak) accepts the value at any index — useful "
                             "when the write index isn't pinned. Strong is also faster on Tofino: "
                             "it decides from a single-index read and never scans the whole array.")
    parser.add_argument("--tamper-value", default="0xdeadbeef")
    parser.add_argument("--tamper-mode", choices=("key", "cond", "both"), required=True,
                        help="which state-dependency tampering policy to generate: "
                             "key=STATE_DEP_TAMPERING (Write-Key chains), "
                             "cond=STATE_DEP_TAMPERING_COND (Write-Condition chains), "
                             "both=run each. Output goes to a per-policy subdir of protobuf/ (bmv2) "
                             "or bfrt/ (tofino). No default — choose explicitly.")
    parser.add_argument("--max-tests", type=int, default=300)
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(2, (os.cpu_count() or 2) // 2),
                        help="parallel workers for gen/compile stages "
                             "(test stage is always single-process)")
    parser.add_argument("--no-ui", action="store_true",
                        help="disable the live 3-line status display")
    parser.add_argument("-v", "--verbose", action="count", default=0)
    args = parser.parse_args()

    # Defer heavy imports until after --help so missing dependencies don't block the usage
    # message, and so Tofino-only environments never need the BMv2 python stack.
    if args.target == "bmv2":
        load_bmv2_deps()

    try:
        from builders import BuilderBase, BMv2Builder, TofinoBuilder
    except ImportError as e:
        sys.exit(f"cannot import builders (expected in {Path(__file__).parent}/builders.py): {e}")

    # Resolve target-specific defaults.
    if args.stage is None:
        args.stage = "all" if args.target == "bmv2" else "gen"
    if args.arch is None:
        args.arch = "v1model" if args.target == "bmv2" else "tna"

    out_root = Path(args.output_root).expanduser()
    out_root.mkdir(parents=True, exist_ok=True)
    csv_path = Path(args.csv).expanduser() if args.csv else out_root / "tampering_results.csv"

    if args.p4_file:
        # Single ad-hoc program — skip program_list.
        p4_path = str(Path(args.p4_file).expanduser())
        name = args.target_name or Path(args.p4_file).stem
        targets = [TargetSpec(name=name, p4_file=p4_path, extra_args=[])]
    else:
        program_list = Path(args.program_list).expanduser()
        targets = parse_program_list(program_list, args.target, args.arch) \
            if program_list.exists() else []
        if args.filter:
            targets = [t for t in targets if args.filter in t.name]
        # For --stage run without --p4-file: fall back to discovering targets
        # from existing output directories so the user can run
        #   --target tofino --filter <name> --stage run
        # without needing a matching program-list entry.
        if not targets and args.stage == "run":
            txtpb_subdir = "bfrt" if args.target == "tofino" else "protobuf"
            for d in sorted(out_root.iterdir()):
                if not d.is_dir():
                    continue
                if args.filter and args.filter not in d.name:
                    continue
                if any((d / txtpb_subdir).rglob("*.txtpb")):
                    targets.append(TargetSpec(name=d.name, p4_file="", extra_args=[]))
            if targets:
                print(f"[run stage] discovered {len(targets)} target(s) from {out_root}",
                      file=sys.stderr)
    _apply_timeout_overrides(targets, args)

    if not targets:
        print("no targets after filtering", file=sys.stderr)
        return 0
    args.targets = targets  # exposed for bmv2_driver/run_bmv2_pipeline

    # Decide log destination. With the live UI, route logs to a file so they
    # don't tear up the three status lines; without UI, log to stderr as before.
    ui_enabled = (not args.no_ui) and sys.stdout.isatty()
    level = logging.INFO - 10 * args.verbose
    log_level = max(level, logging.DEBUG)
    log_handlers: List[logging.Handler] = []

    if ui_enabled:
        log_path = out_root / "tampering.log"
        log_handlers.append(logging.FileHandler(log_path, mode="w"))
    else:
        log_handlers.append(logging.StreamHandler(sys.stderr))
    logging.basicConfig(
        level=log_level, handlers=log_handlers,
        format="%(asctime)s %(levelname)s %(message)s", force=True,
    )

    # sudo + dataplane cleanup are only needed when we exercise the data plane
    # (run/all stage); gen stage doesn't shell out as root.
    need_sudo = args.stage in ("run", "all") and args.target in ("bmv2", "tofino")
    if need_sudo:
        # Prompt for sudo once so later sudo invocations are non-interactive.
        subprocess.run(["sudo", "-v"], check=False)
        # Make sure any leftover simple_switch_grpc is gone before we start.
        subprocess.run(["sudo", "pkill", "-KILL", "-f", "simple_switch_grpc"],
                       check=False, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       start_new_session=True, timeout=10)

    # ^C → graceful shutdown across all stages and subprocesses.
    signal.signal(signal.SIGINT, _sigint_handler)
    signal.signal(signal.SIGTERM, _sigint_handler)

    counts = {"OK": 0, "FAIL": 0, "ERROR": 0, "SKIP": 0}
    new_file = not csv_path.exists()
    csv_lock = threading.Lock()
    cf = csv_path.open("a", newline="")
    writer = csv.writer(cf, quoting=csv.QUOTE_NONNUMERIC)
    if new_file:
        writer.writerow(["target_name", "protobuf_file", "result", "reason"])
        cf.flush()

    def csv_emit(target_name: str, protobuf_file: str, result: str, reason: str) -> None:
        with csv_lock:
            counts[result] = counts.get(result, 0) + 1
            writer.writerow([target_name, protobuf_file, result, reason])
            cf.flush()

    status = Status(total=len(targets))

    renderer = Renderer(status, enabled=True, target=args.target) if ui_enabled else None
    if renderer:
        renderer.start()
    try:
        if args.target == "bmv2":
            builder = BMv2Builder()
        elif args.target == "tofino":
            builder = TofinoBuilder()
        else:
            sys.exit(f"unknown target: {args.target}")

        pipeline = Pipeline(targets, args, csv_emit, status, builder=builder)
        pipeline.run()
    except KeyboardInterrupt:
        log.warning("interrupted by user")
        request_shutdown()
    finally:
        if renderer:
            renderer.stop()
        cf.close()
        # Final safety net: make sure nothing privileged we spawned is still alive.
        if SHUTDOWN.is_set():
            request_shutdown()  # idempotent re-issue of pkill / tracked-kill
        # Always clean up tofino-model / bf_switchd regardless of how we exit.
        # do_testing()'s finally block calls stop(), but stop() may miss the real
        # binary if the launcher script already exited (new session fork pattern).
        if getattr(args, "target", None) == "tofino" and \
                getattr(args, "sde_manage_procs", False):
            sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
            for proc_name in ("tofino-model", "bf_switchd"):
                try:
                    subprocess.run(sudo + ["pkill", "-KILL", "-f", proc_name],
                                   check=False, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL,
                                   start_new_session=True, timeout=5)
                except Exception:
                    pass

    # Per-stage breakdown — tells the user which step things died at.
    n = status.total
    print("summary:")
    print(f"  step (1) gen (p4symbex):    {status.gen_ok}/{n} OK, {status.gen_err} errors")
    print(f"  step (2) compile (p4c):     {status.compile_ok}/{n} OK, {status.compile_err} errors")
    print(f"  step (3) {args.target} testing: {status.test_ok}/{n} OK, "
          f"{status.test_fail} FAIL, {status.test_err} errors")
    print(f"  per-protobuf rows:          OK={counts.get('OK', 0)} FAIL={counts.get('FAIL', 0)} "
          f"ERROR={counts.get('ERROR', 0)} SKIP={counts.get('SKIP', 0)}")
    print(f"  csv: {csv_path}")
    if ui_enabled:
        print(f"  log: {out_root / 'tampering.log'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
