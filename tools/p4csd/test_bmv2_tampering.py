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

try:
    from p4.v1 import p4runtime_pb2
    from p4.v1 import p4data_pb2
    from p4.config.v1 import p4info_pb2
    from google.protobuf import text_format
except ImportError as e:
    sys.exit(f"missing P4Runtime python bindings: {e}")

try:
    from scapy.all import Ether, sendp, rdpcap, conf as scapy_conf
except ImportError as e:
    sys.exit(f"missing scapy: {e}")

# Hardcoded so that running under `sudo` (which sets HOME=/root) still resolves
# the well-known paths correctly.
HOME = Path("/home/vagrant")

# Bundled minimal P4Runtime client (see p4_client.py next to this file).
sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    from p4_client import P4RuntimeClient, P4RuntimeWriteException
    from bmv2_thrift_client import Bmv2ThriftClient
except ImportError as e:
    sys.exit(f"cannot import p4_client (expected at {Path(__file__).parent}): {e}")
P4C_BIN = HOME / "Workspace/p4c/build/p4c"
P4SYMBEX_BIN = HOME / "Workspace/p4c/build/p4symbex"
SIMPLE_SWITCH_GRPC = shutil.which("simple_switch_grpc") or "simple_switch_grpc"

GRPC_HOST = "127.0.0.1"  # localhost may resolve to ::1 only; BMv2 listens IPv4
GRPC_PORT = 9559
THRIFT_PORT = 9090
DEVICE_ID = 0  # simple_switch_grpc defaults to device_id 0
ELECTION_ID = (0, 1)

# Switch sees veth{2N} (even); host injects/sniffs on veth{2N+1} (odd).
NUM_PORTS = 8
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
    for name in ("simple_switch_grpc", "tcpdump"):
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
    extra_args: List[str] = field(default_factory=list)


def parse_program_list(path: Path) -> List[TargetSpec]:
    targets: List[TargetSpec] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            toks = line.split()
            if len(toks) < 4:
                continue
            name, p4_file, target, p4_version = toks[:4]
            if target != "v1model" or p4_version != "16":
                continue
            extras = [t for t in toks[4:] if t != "$@"]
            targets.append(TargetSpec(
                name=name,
                p4_file=os.path.expanduser(p4_file),
                extra_args=[os.path.expanduser(a) for a in extras],
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


def run_p4c(name: str, p4_file: str, extra_args: List[str], build_dir: Path) -> Tuple[Path, Path]:
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
        "--std", "p4-16",
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
                 max_tests: int, tamper_value: str, skip: bool) -> List[Path]:
    if skip and protobuf_dir.exists():
        files = sorted(protobuf_dir.glob("*.txtpb"))
        if files:
            log.info("[%s] reusing %d existing txtpb files", name, len(files))
            return files
    if protobuf_dir.exists():
        for f in protobuf_dir.glob("*.txtpb"):
            f.unlink()
    protobuf_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(P4SYMBEX_BIN),
        "--target", "bmv2",
        "--std", "p4-16",
        "--arch", "v1model",
        "--test-backend", "protobuf",
        "--packet-size-range", "0:9600",
        "--track-coverage", "STATEMENTS",
        "--max-tests", str(max_tests),
        "--max-port", str(NUM_PORTS),
        "--out-dir", str(protobuf_dir),
        "--state-dep",
        "--path-selection", "STATE_DEP_TAMPERING",
        p4_file,
        "--state-tamper-value", tamper_value,
    ]
    _run(cmd, log_path=protobuf_dir.parent / "p4symbex.log", timeout=1800)
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
        for _ in range(attempts):
            if self.proc and self.proc.poll() is not None:
                raise StepError(
                    f"simple_switch_grpc exited early; see {self.log_path}")
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                try:
                    s.connect((GRPC_HOST, GRPC_PORT))
                    return
                except OSError:
                    time.sleep(delay)
        raise StepError(f"grpc port {GRPC_PORT} did not come up (see {self.log_path})")

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
class TamperCase:
    phases: List[Phase]
    entities: List[p4runtime_pb2.Entity]
    path: str
    affected_registers: List[dict] = field(default_factory=list)


_BLOCK_RE = re.compile(r"^(\w+)\s*\{\s*$")
_PACKET_RE = re.compile(r'packet:\s*"((?:[^"\\]|\\.)*)"')
_MASK_RE = re.compile(r'packet_mask:\s*"((?:[^"\\]|\\.)*)"')
_PORT_RE = re.compile(r"port:\s*(\d+)")
_REG_NAME_RE = re.compile(r'register_name:\s*"([^"]*)"')
_REG_IDX_RE = re.compile(r'\bindex:\s*(\d+)')
_REG_VAL_RE = re.compile(r'attacker_value:\s*"((?:[^"\\]|\\.)*)"')


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
                affected_registers.append({
                    "name": name_m.group(1),
                    "index": int(idx_m.group(1)),
                    "attacker_value": val_int,
                })
        # ignore unknown headers (e.g. metadata/traces at top level are not blocks)

    if current is not None:
        phases.append(current)

    while len(phases) < 3:
        # Tolerate malformed files — pad with empty phases that will FAIL noisily.
        phases.append(Phase(in_packet=b"", in_port=0))
    return TamperCase(phases=phases[:3], entities=entities, path=str(path),
                      affected_registers=affected_registers)


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
                        try:
                            # p4_client.py's delete dispatches on TableEntry, not Entity
                            client.delete(ent.table_entry)
                        except P4RuntimeWriteException as ex:
                            log.debug("delete failed (ok if const): %s", ex)
        except Exception as ex:  # pragma: no cover — defensive
            log.debug("read failed for table %s: %s", table.preamble.name, ex)


def clear_all_registers(
        client: P4RuntimeClient,
        p4info: p4info_pb2.P4Info,
        thrift_client: "Optional[Bmv2ThriftClient]" = None) -> None:
    if thrift_client is not None:
        for reg in p4info.registers:
            try:
                thrift_client.clear_register(reg.preamble.name)
            except Exception as ex:
                log.debug("thrift register clear failed for %s: %s",
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

def install_entities(client: P4RuntimeClient, entities: List[p4runtime_pb2.Entity]) -> None:
    for ent in entities:
        try:
            # p4_client.py's insert dispatches on TableEntry, not Entity
            client.insert(ent.table_entry)
        except P4RuntimeWriteException as ex:
            msg = str(ex)
            if "ALREADY_EXISTS" in msg:
                continue
            raise


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

scapy_conf.verb = 0


def _masked_eq(actual: bytes, expected: bytes, mask: bytes) -> bool:
    n = len(expected)
    if len(actual) < n:
        return False
    a = bytes(x & m for x, m in zip(actual[:n], mask))
    e = bytes(x & m for x, m in zip(expected, mask))
    return a == e


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
    def __init__(self, phase_timeout: float, capture_dir: Path, thrift_client=None):
        self.phase_timeout = phase_timeout
        self.capture_dir = capture_dir
        self.thrift_client = thrift_client
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
                actual = vals[idx] if idx < len(vals) else None
                if actual != expected:
                    return False, (
                        f"register {reg['name']}[{idx}]="
                        f"{'None' if actual is None else hex(actual)}, "
                        f"expected {hex(expected)}"
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
    return run_p4c(spec.name, spec.p4_file, spec.extra_args, build_dir)


def do_p4symbex(spec: TargetSpec, args) -> List[Path]:
    out_root = Path(args.output_root).expanduser()
    protobuf_dir = out_root / spec.name / "protobuf"
    return run_p4symbex(
        spec.name, spec.p4_file, protobuf_dir,
        max_tests=args.max_tests,
        tamper_value=args.tamper_value,
        skip=args.skip_p4symbex,
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
    ok_count = 0
    fail_count = 0
    err_count = 0

    def emit(protobuf_file, result, reason):
        nonlocal ok_count, fail_count, err_count
        csv_writer(spec.name, protobuf_file, result, reason)
        if result == "OK":
            ok_count += 1
        elif result == "FAIL":
            fail_count += 1
        else:
            err_count += 1

    try:
        with bmv2_session(json_path, target_root / "bmv2.log", args.keep_bmv2):
            log.info("[%s] connecting P4Runtime client", spec.name)
            client = P4RuntimeClient(
                device_id=DEVICE_ID,
                grpc_address=f"{GRPC_HOST}:{GRPC_PORT}",
                election_id=ELECTION_ID,
            )
            thrift_client = Bmv2ThriftClient(GRPC_HOST, THRIFT_PORT)
            if thrift_client.is_alive():
                log.info("[%s] Thrift client active on %s:%d",
                         spec.name, GRPC_HOST, THRIFT_PORT)
            else:
                log.warning("[%s] Thrift client connected but probe failed on %s:%d",
                            spec.name, GRPC_HOST, THRIFT_PORT)
            try:
                client.set_fwd_pipe_config(str(p4info_path), str(json_path))
                p4info = load_p4info(client)
                tester = PacketTester(args.phase_timeout, capture_dir, thrift_client=thrift_client)
                total = len(txtpb_files)
                all_regs = get_registers(client, p4info, thrift_client=thrift_client)
                if len(all_regs) == 0:
                    log.error("[%s] no registers found in P4Info; is this really a v1model target?", spec.name)
                for idx, tx in enumerate(txtpb_files, 1):
                    if SHUTDOWN.is_set():
                        break
                    progress_cb(idx - 1, total, tx.name)  # report "starting"
                    try:
                        clear_all_entries(client, p4info)
                        clear_all_registers(client, p4info, thrift_client=thrift_client)
                        installed = read_entities(client, p4info)
                        if len(installed) > 0:
                            raise StepError(
                                f"{len(installed)} entities remain after clear"
                            )
                        nonzero_regs = read_nonzero_registers(
                            client, p4info, thrift_client=thrift_client)
                        if nonzero_regs:
                            raise StepError(
                                f"{len(nonzero_regs)} register cells"
                                " non-zero after clear"
                            )
                        case = parse_tampering_txtpb(tx)
                        install_entities(client, case.entities)
                        installed = read_entities(client, p4info)
                        if len(installed) != len(case.entities):
                            raise StepError(
                                f"install mismatch: requested {len(case.entities)},"
                                f" installed {len(installed)}"
                            )
                        log.info("[%s] installed %d entities for %s",
                                 spec.name, len(installed), tx.name)

                    except P4RuntimeWriteException as ex:
                        emit(tx.name, "ERROR", f"install: {ex}")
                        progress_cb(idx, total, tx.name)
                        continue
                    except Exception as ex:
                        emit(tx.name, "ERROR", f"setup: {ex}")
                        progress_cb(idx, total, tx.name)
                        continue
                    try:
                        ok, reason = tester.run(case, label=tx.stem)
                    except Exception as ex:
                        emit(tx.name, "ERROR", f"replay: {ex}")
                        progress_cb(idx, total, tx.name)
                        continue
                    emit(tx.name, "OK" if ok else "FAIL", reason)
                    progress_cb(idx, total, tx.name)
            finally:
                client.tear_down()
                thrift_client.close()
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
        self.build_done = 0
        self.build_ok = 0
        self.build_err = 0
        self.build_active: List[str] = []
        self.symbex_done = 0
        self.symbex_ok = 0
        self.symbex_err = 0
        self.symbex_active: List[str] = []
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

    def render(self) -> Tuple[str, str, str]:
        with self.lock:
            t = self.total
            pct = lambda n: (100.0 * n / t) if t else 0.0
            l1 = (f"(1) p4c build:    {self.build_ok}/{t} "
                  f"({pct(self.build_ok):.0f}% success, {self.build_err} errors)"
                  f"  building {self._fmt_active(self.build_active)}")
            l2 = (f"(2) p4symbex:     {self.symbex_ok}/{t} "
                  f"({pct(self.symbex_ok):.0f}% success, {self.symbex_err} errors)"
                  f"  running {self._fmt_active(self.symbex_active)}")
            tx_info = "-"
            if self.test_target:
                if self.test_tx_total:
                    tx_pct = 100.0 * self.test_tx_idx / self.test_tx_total
                    tx_info = (f"{self.test_target} "
                               f"[{self.test_tx_name} "
                               f"({self.test_tx_idx}/{self.test_tx_total}: {tx_pct:.0f}%)]")
                else:
                    tx_info = self.test_target
            l3 = (f"(3) BMv2 testing: {self.test_ok}/{t} "
                  f"({pct(self.test_ok):.0f}% success, "
                  f"{self.test_fail} fail, {self.test_err} errors)"
                  f"  testing {tx_info}")
            return l1, l2, l3


class Renderer:
    """Repaints three TUI lines in place using ANSI escape codes."""

    def __init__(self, status: Status, enabled: bool, period: float = 0.2):
        self.status = status
        self.enabled = enabled and sys.stdout.isatty()
        self.period = period
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def _draw(self, first: bool) -> None:
        l1, l2, l3 = self.status.render()
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
    """Pipelined runner: build pool → symbex pool → single test worker."""

    def __init__(self, targets: List[TargetSpec], args, csv_writer_fn, status: Status):
        self.targets = targets
        self.args = args
        self.csv_writer = csv_writer_fn
        self.status = status

    def _csv(self, *row) -> None:
        self.csv_writer(*row)

    def _build_task(self, spec: TargetSpec) -> None:
        if SHUTDOWN.is_set():
            return
        with self.status.lock:
            self.status.build_active.append(spec.name)
        try:
            json_path, p4info_path = do_build(spec, self.args)
            with self.status.lock:
                self.status.build_done += 1
                self.status.build_ok += 1
                if spec.name in self.status.build_active:
                    self.status.build_active.remove(spec.name)
            self._symbex_q.put(("ok", spec, json_path, p4info_path))
        except StepError as ex:
            with self.status.lock:
                self.status.build_done += 1
                self.status.build_err += 1
                self.status.symbex_done += 1
                self.status.test_done += 1
                if spec.name in self.status.build_active:
                    self.status.build_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"p4c: {ex}")
        except Exception as ex:  # defensive
            with self.status.lock:
                self.status.build_done += 1
                self.status.build_err += 1
                self.status.symbex_done += 1
                self.status.test_done += 1
                if spec.name in self.status.build_active:
                    self.status.build_active.remove(spec.name)
            self._csv(spec.name, "", "ERROR", f"p4c-unhandled: {ex}")

    def _symbex_worker(self) -> None:
        while True:
            item = self._symbex_q.get()
            if item is None:
                return
            if SHUTDOWN.is_set():
                continue  # drain queue without doing work
            _, spec, json_path, p4info_path = item
            with self.status.lock:
                self.status.symbex_active.append(spec.name)
            try:
                txtpb_files = do_p4symbex(spec, self.args)
                ok = bool(txtpb_files)
                with self.status.lock:
                    self.status.symbex_done += 1
                    if ok:
                        self.status.symbex_ok += 1
                    else:
                        self.status.symbex_err += 1
                    if spec.name in self.status.symbex_active:
                        self.status.symbex_active.remove(spec.name)
                if not ok:
                    with self.status.lock:
                        self.status.test_done += 1
                    self._csv(spec.name, "", "ERROR", "p4symbex produced no .txtpb files")
                    continue
                self._test_q.put((spec, json_path, p4info_path, txtpb_files))
            except StepError as ex:
                with self.status.lock:
                    self.status.symbex_done += 1
                    self.status.symbex_err += 1
                    self.status.test_done += 1
                    if spec.name in self.status.symbex_active:
                        self.status.symbex_active.remove(spec.name)
                self._csv(spec.name, "", "ERROR", f"p4symbex: {ex}")
            except Exception as ex:
                with self.status.lock:
                    self.status.symbex_done += 1
                    self.status.symbex_err += 1
                    self.status.test_done += 1
                    if spec.name in self.status.symbex_active:
                        self.status.symbex_active.remove(spec.name)
                self._csv(spec.name, "", "ERROR", f"p4symbex-unhandled: {ex}")

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
                ok_count, fail_count, err_count = do_testing(
                    spec, json_path, p4info_path, txtpb_files,
                    self.args, self.csv_writer, progress_cb,
                )
            except Exception as ex:
                ok_count, fail_count, err_count = 0, 0, 1
                self._csv(spec.name, "", "ERROR", f"test-unhandled: {ex}")

            with self.status.lock:
                self.status.test_done += 1
                # target-level classification (only ONE bucket per target):
                #   ERROR if any infrastructure failure was hit
                #   OK    if every protobuf passed
                #   FAIL  otherwise (at least one verification mismatch)
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

    def run(self) -> None:
        self._symbex_q: "queue.Queue" = queue.Queue()
        self._test_q: "queue.Queue" = queue.Queue()
        jobs = max(1, self.args.jobs)
        bpool = ThreadPoolExecutor(max_workers=jobs, thread_name_prefix="build")
        symbex_pool = ThreadPoolExecutor(max_workers=jobs, thread_name_prefix="symbex")
        for _ in range(jobs):
            symbex_pool.submit(self._symbex_worker)
        test_thread = threading.Thread(target=self._test_worker, name="test", daemon=True)
        test_thread.start()

        try:
            for spec in self.targets:
                if SHUTDOWN.is_set():
                    break
                bpool.submit(self._build_task, spec)
            bpool.shutdown(wait=True)  # all build tasks complete
        except KeyboardInterrupt:
            # main thread got ^C — drop pending builds, drain via sentinels
            try:
                bpool.shutdown(wait=False, cancel_futures=True)
            except TypeError:
                bpool.shutdown(wait=False)
        finally:
            # always send sentinels so workers exit even when interrupted
            for _ in range(jobs):
                self._symbex_q.put(None)
            try:
                symbex_pool.shutdown(wait=True)
            except KeyboardInterrupt:
                symbex_pool.shutdown(wait=False)
            self._test_q.put(None)
            # Wait indefinitely for normal completion. On SHUTDOWN, the worker
            # checks the flag between protobufs and bails out fast, so this join
            # still returns promptly.
            test_thread.join()


# --------------------------------------------------------------------------- #
# Main                                                                        #
# --------------------------------------------------------------------------- #

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program-list",
                        default=str(HOME / "Workspace-remote/top_tier_repo/program_list.txt"))
    parser.add_argument("--output-root",
                        default=str(HOME / "Workspace-remote/top_tier_repo/output"))
    parser.add_argument("--csv", default=None,
                        help="CSV path (default: <output-root>/test_bmv2_tampering_results.csv)")
    parser.add_argument("--filter", default=None,
                        help="substring match on target name; runs only matching rows")
    parser.add_argument("--skip-p4symbex", action="store_true",
                        help="reuse existing .txtpb files instead of regenerating")
    parser.add_argument("--keep-bmv2", action="store_true",
                        help="leave BMv2 running between targets (debug)")
    parser.add_argument("--phase-timeout", type=float, default=2.0)
    parser.add_argument("--tamper-value", default="0xdeadbeef")
    parser.add_argument("--max-tests", type=int, default=300)
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(2, (os.cpu_count() or 2) // 2),
                        help="parallel workers for build/p4symbex stages "
                             "(test stage is always single-process)")
    parser.add_argument("--no-ui", action="store_true",
                        help="disable the live 3-line status display")
    parser.add_argument("-v", "--verbose", action="count", default=0)
    args = parser.parse_args()

    program_list = Path(args.program_list).expanduser()
    out_root = Path(args.output_root).expanduser()
    out_root.mkdir(parents=True, exist_ok=True)
    csv_path = Path(args.csv).expanduser() if args.csv else out_root / "test_bmv2_tampering_results.csv"

    targets = parse_program_list(program_list)
    if args.filter:
        targets = [t for t in targets if args.filter in t.name]
    if not targets:
        print("no targets after filtering", file=sys.stderr)
        return 0

    # Decide log destination. With the live UI, route logs to a file so they
    # don't tear up the three status lines; without UI, log to stderr as before.
    ui_enabled = (not args.no_ui) and sys.stdout.isatty()
    level = logging.INFO - 10 * args.verbose
    log_level = max(level, logging.DEBUG)
    log_handlers: List[logging.Handler] = []

    if ui_enabled:
        log_path = out_root / "test_bmv2_tampering.log"
        log_handlers.append(logging.FileHandler(log_path, mode="w"))
    else:
        log_handlers.append(logging.StreamHandler(sys.stderr))
    logging.basicConfig(
        level=log_level, handlers=log_handlers,
        format="%(asctime)s %(levelname)s %(message)s", force=True,
    )

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

    counts = {"OK": 0, "FAIL": 0, "ERROR": 0}
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
    pipeline = Pipeline(targets, args, csv_emit, status)

    renderer = Renderer(status, enabled=True) if ui_enabled else None
    if renderer:
        renderer.start()
    try:
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

    # Per-stage breakdown — tells the user which step things died at.
    n = status.total
    print("summary:")
    print(f"  step (1) p4c build:    {status.build_ok}/{n} OK, {status.build_err} errors")
    print(f"  step (2) p4symbex:     {status.symbex_ok}/{n} OK, {status.symbex_err} errors")
    print(f"  step (3) BMv2 testing: {status.test_ok}/{n} OK, "
          f"{status.test_fail} FAIL, {status.test_err} errors")
    print(f"  per-protobuf rows:     OK={counts['OK']} FAIL={counts['FAIL']} ERROR={counts['ERROR']}")
    print(f"  csv: {csv_path}")
    if ui_enabled:
        print(f"  log: {out_root / 'test_bmv2_tampering.log'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
