"""Tofino driver for the unified tampering harness.

Mirrors ``bmv2/bmv2_driver.py`` but targets the Tofino backend:
  Stage 1 (build):    p4c (or bf-p4c) compiles the .p4 → context.json + bfrt.json + tofino.bin.
  Stage 2 (generate): p4symbex --target tofino --test-backend BFRT writes .txtpb cases.
  Stage 3 (test):     replays the .txtpb cases against a running tofino_model via BF-Runtime gRPC.

The build/generate stages run on any host with p4c/p4symbex. The test stage
requires the Intel BF-SDE (``tofino_model`` + ``bf_switchd``). When the SDE is
absent, :func:`require_sde` exits with an actionable error message.
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

log = logging.getLogger("tofino_driver")

HOME = Path(os.environ.get("HOME", "/home/vagrant"))

# Defaults can be overridden via CLI flags in tampering.py.
DEFAULT_P4C_BIN = HOME / "Workspace/p4c/build/p4c"
DEFAULT_P4SYMBEX_BIN = HOME / "Workspace/p4c/build/p4symbex"

# SDE binaries we expect on the *test* host.
TOFINO_MODEL_BIN = "tofino_model"
BF_SWITCHD_BIN = "bf_switchd"

# BF-Runtime gRPC listens on 50052 by default on the SDE.
DEFAULT_BFRT_GRPC_PORT = 50052

BFRT_GRPC_HOST = "127.0.0.1"    # mirrors GRPC_HOST in tampering.py
BFRT_GRPC_ADDR = f"{BFRT_GRPC_HOST}:{DEFAULT_BFRT_GRPC_PORT}"
# Mirror of tampering.NICE_ABOVE_SECS; --symbex-timeout-min raises both together so a floored budget
# does not newly nice the whole corpus.
NICE_ABOVE_SECS = 1800

NUM_PORTS = 32
# Even-numbered veth is the host/PTF side; odd-numbered veth is the model side.
# This matches the standard SDE veth_setup.sh pairing convention.
PORT_TO_HOST_IFACE = {n: f"veth{2 * n}" for n in range(NUM_PORTS)}

# ports.json passed to run_tofino_model.sh via -f, mapping device ports 0-31
# to their veth pairs (port N → veth{2N}/veth{2N+1}).
_PORTS_JSON_PATH = Path(__file__).parent / "ports.json"


def _veth_exists(name: str) -> bool:
    return Path(f"/sys/class/net/{name}").exists()


def _ensure_veth_pair(host_v: int, model_v: int) -> bool:
    """Create + bring up a veth pair (idempotent), disabling offloads like the
    SDE's veth_setup.sh. Returns True if the pair is present afterwards.

    Used for out-of-range device ports that the standard veth_setup.sh did not
    provision (e.g. a program that hardcodes egress to port 140 → veth280/281)."""
    host, model = f"veth{host_v}", f"veth{model_v}"
    if not _veth_exists(host):
        subprocess.run(["sudo", "ip", "link", "add", host, "type", "veth",
                        "peer", "name", model],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for v in (host, model):
        subprocess.run(["sudo", "ip", "link", "set", "dev", v, "up"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["sudo", "ip", "link", "set", "dev", v, "mtu", "10240"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if shutil.which("ethtool"):
            subprocess.run(["sudo", "ethtool", "--offload", v, "rx", "off", "tx", "off",
                            "sg", "off", "tso", "off", "gso", "off", "gro", "off", "lro", "off"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return _veth_exists(host) and _veth_exists(model)


def prepare_ports_json(used_ports, work_dir: Path) -> Tuple[Path, dict]:
    """Ensure every device port referenced by the cases has a veth pair and a
    ports.json entry so the model can forward to it and the harness can monitor it.

    Some programs hardcode an egress port outside the standard 0-31 range (e.g.
    ACC-Turbo forwards everything to port 140); without an entry the model has no
    interface to emit on and the forward-vs-drop divergence is unobservable.

    Returns (ports_json_path, {device_port: host_veth_iface}). When no extra ports
    are needed, returns the static ports.json untouched."""
    data = json.loads(_PORTS_JSON_PATH.read_text())
    entries = data.get("PortToVeth", [])
    have = {e["device_port"] for e in entries}
    extra = sorted(p for p in used_ports if p not in have and p >= 0)

    def _host_map(es):
        return {e["device_port"]: f"veth{e['veth1']}" for e in es}

    if not extra:
        return _PORTS_JSON_PATH, _host_map(entries)

    for p in extra:
        host_v, model_v = 2 * p, 2 * p + 1
        if not _ensure_veth_pair(host_v, model_v):
            log.warning("could not provision veth pair for out-of-range port %s "
                        "(veth%s/veth%s); egress to it stays unobservable", p, host_v, model_v)
        entries.append({"device_port": p, "veth1": host_v, "veth2": model_v})

    data["PortToVeth"] = entries
    work_dir.mkdir(parents=True, exist_ok=True)
    out = work_dir / "ports.gen.json"
    out.write_text(json.dumps(data, indent=4))
    log.info("provisioned out-of-range ports %s → %s", extra, out)
    return out, _host_map(entries)

# --------------------------------------------------------------------------- #
# SDE detection                                                               #
# --------------------------------------------------------------------------- #

def require_sde() -> None:
    """Exit with a clear error message if the BF-SDE binaries are missing.

    The build/generate stages don't need the SDE, but the test stage replays
    against a live tofino_model and can't proceed without it.
    """
    missing = [b for b in (TOFINO_MODEL_BIN, BF_SWITCHD_BIN)
               if shutil.which(b) is None]
    if not missing:
        return
    sys.exit(
        "Tofino --stage test requires the Intel BF-SDE in PATH.\n"
        f"Missing binaries: {', '.join(missing)}.\n"
        "Run `--stage build` on this host, copy the output directory to the\n"
        "SDE host (where tofino_model and bf_switchd are installed), and re-run\n"
        "the same command with `--stage test` there."
    )


# --------------------------------------------------------------------------- #
# Step helpers — kept independent of tampering.py to avoid circular imports   #
# --------------------------------------------------------------------------- #

class TofinoStepError(RuntimeError):
    pass


def _run(cmd: List[str], log_path: Optional[Path] = None, timeout: int = 1800,
         cwd: Optional[Path] = None) -> None:
    log.debug("exec: %s", " ".join(str(c) for c in cmd))
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w") as lf:
            proc = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                                  stdout=lf, stderr=subprocess.STDOUT,
                                  cwd=cwd, start_new_session=True, timeout=timeout)
    else:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True,
                              cwd=cwd, start_new_session=True, timeout=timeout)
    if proc.returncode != 0:
        tail = ""
        if log_path is not None and log_path.exists():
            tail = log_path.read_text()[-2000:]
        elif hasattr(proc, "stdout"):
            tail = (proc.stdout or "") + (proc.stderr or "")
        raise TofinoStepError(
            f"command failed ({proc.returncode}): {' '.join(str(c) for c in cmd)}\n{tail[-1000:]}")


# Default include paths and target defines needed by both p4c and p4symbex
# when the user passes a TNA program directly (no harness includes baked in).
P4C_INCLUDE_FLAGS = [
    "-I" + str(HOME / "Workspace/p4c/backends/tofino/bf-p4c/p4include"),
    "-I" + str(HOME / "Workspace/p4c/p4include"),
]


def _target_define(arch: str) -> str:
    return "-D__TARGET_TOFINO__=2" if arch in ("t2na", "tofino2") else "-D__TARGET_TOFINO__=1"


def run_p4c(name: str, p4_file: str, extra_args: List[str], build_dir: Path,
            arch: str = "tna", p4c_bin: Path = DEFAULT_P4C_BIN,
            p4_version: str = "p4-16", force: bool = False) -> Path:
    """Compile a TNA .p4 program. Returns the directory of the produced
    artifacts (context.json, bfrt.json, tofino.bin live here).

    ``force`` recompiles even when artifacts are present. The presence check below is
    not a staleness check — it never compares mtimes, and it could not see edits to
    #included headers even if it did — so a modified program otherwise replays its old
    binary silently.
    """
    build_dir.mkdir(parents=True, exist_ok=True)

    def _has_artifacts(d: Path) -> bool:
        """A successful Tofino compile leaves either bfrt.json (typical
        bf-p4c output) OR pipe/context.json (intermediate p4c-barefoot output
        when bfrt schema generation is disabled)."""
        if list(d.rglob("bfrt.json")):
            return True
        if list(d.rglob("context.json")):
            return True
        return False

    if _has_artifacts(build_dir) and not force:
        log.info("[%s] tofino artifacts present, skipping p4c (use --force-compile to rebuild)",
                 name)
        return build_dir
    if force and _has_artifacts(build_dir):
        log.info("[%s] --force-compile: rebuilding over existing artifacts in %s", name, build_dir)
    cmd = [
        str(p4c_bin),
        "--target", "tofino",
        "--arch", arch,
        "--std", p4_version,
        _target_define(arch),
        *P4C_INCLUDE_FLAGS,
        "-o", str(build_dir),
        *extra_args,
        p4_file,
    ]
    _run(cmd, log_path=build_dir / "p4c.log")
    if not _has_artifacts(build_dir):
        raise TofinoStepError(
            f"p4c produced no recognizable tofino artifacts under {build_dir} "
            "(expected bfrt.json or pipe/context.json)")
    return build_dir


def run_p4symbex(name: str, p4_file: str, txtpb_dir: Path, *, arch: str = "tna",
                 max_tests: int, tamper_value: str, skip: bool,
                 path_selection: str = "STATE_DEP_TAMPERING",
                 p4symbex_bin: Path = DEFAULT_P4SYMBEX_BIN,
                 p4_version: str = "p4-16",
                 extra_args: List[str] = [],
                 state_dep_cache: "Optional[Path]" = None,
                 cp_annotation: "Optional[Path]" = None,
                 shared_traversal: "Optional[str]" = None,
                 chain_impact_order: bool = False,
                 timeout: int = 1800) -> List[Path]:
    """Run p4symbex with the BFRT backend to emit .txtpb tampering cases.

    When @state_dep_cache is given, p4symbex loads pre-computed SOChains from it (--state-dep-cache)
    instead of re-running the IFDS analysis; it hard-errors if the file is missing or stale."""
    if skip and txtpb_dir.exists():
        files = sorted(txtpb_dir.glob("*.txtpb"))
        if files:
            log.info("[%s] reusing %d existing txtpb files", name, len(files))
            return files
    if txtpb_dir.exists():
        for f in txtpb_dir.glob("*.txtpb"):
            f.unlink()
    txtpb_dir.mkdir(parents=True, exist_ok=True)
    # Lower CPU priority for expensive (above-default timeout) programs so they don't hog the
    # scheduler when they overlap cheap ones at the tail of the pool.
    nice_prefix = ["nice", "-n", "10"] if timeout > NICE_ABOVE_SECS else []
    cmd = [
        *nice_prefix,
        str(p4symbex_bin),
        "--target", "tofino",
        "--arch", arch,
        "--std", p4_version,
        _target_define(arch),
        *P4C_INCLUDE_FLAGS,
        "--test-backend", "BFRT",
        # Min 14 bytes = one Ethernet header; below this scapy's Ether() cannot dissect the
        # frame and the replay send fails. Avoids p4symbex emitting sub-Ethernet packets.
        "--packet-size-range", "14:9600",
        "--track-coverage", "STATEMENTS",
        "--max-tests", str(max_tests),
        "--max-port", str(NUM_PORTS),
        "--out-dir", str(txtpb_dir),
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
    _run(cmd, log_path=txtpb_dir.parent / f"p4symbex.{path_selection}.log", timeout=timeout)
    return sorted(txtpb_dir.glob("*.txtpb"))


def get_tofino_artifacts(build_dir: Path) -> Tuple[Optional[Path], Optional[Path]]:
    """Discover primary Tofino build artifacts (bfrt.json, context.json).

    Returns (bfrt_json_path, context_json_path) or (None, None) if not found.
    Used by builders.py TofinoBuilder to return the Tuple[Path, Path] interface.
    """
    bfrt_json = next(build_dir.rglob("bf-rt.json"), None)
    pipe_dir = next(build_dir.rglob("pipe"), None)
    context_json = next(pipe_dir.rglob("context.json"), None)
    return bfrt_json, context_json


# --------------------------------------------------------------------------- #
# SDE cmake build                                                             #
# --------------------------------------------------------------------------- #

def run_sde_cmake_build(name: str, p4_file: str, output_root: Path,
                        extra_args: Optional[List[str]] = None,
                        force: bool = False) -> Path:
    """Build a TNA program via cmake + make install using the SDE p4studio.

    The SDE's run_tofino_model.sh / run_switchd.sh expect the compiled program
    to live under $SDE_INSTALL/share/tofinopd/<name>/. This function achieves
    that via the standard 'cmake $SDE/p4studio … && make install' workflow.

    ``extra_args`` are the per-program preprocessor flags from the program-list
    row (e.g. ``-I <dir>`` include paths, ``-D<def>`` defines). The SDE p4studio
    cmake forwards its ``P4PPFLAGS`` variable to the bf-p4c preprocessor, so we
    pass them through there — otherwise SketchLib/fabric-tna style programs fail
    to find their headers under the cmake build path.
    """
    sde = os.environ.get("SDE")
    sde_install = os.environ.get("SDE_INSTALL")
    if not sde or not sde_install:
        raise TofinoStepError(
            "$SDE and $SDE_INSTALL must be set for cmake-based build. "
            "Source the SDE environment first."
        )
    sde_path = Path(sde)
    sde_install_path = Path(sde_install)
    installed_dir = sde_install_path / "share" / "tofinopd" / name

    # Smart-skip: already installed with a .bin artifact. Presence only -- it cannot tell a
    # stale binary from a current one, so --force-compile must be able to defeat it.
    if installed_dir.exists() and list(installed_dir.rglob("*.bin")) and not force:
        log.info("[%s] cmake artifacts already installed at %s, skipping "
                 "(use --force-compile to rebuild)", name, installed_dir)
        return installed_dir
    if force and installed_dir.exists():
        log.info("[%s] --force-compile: reinstalling over %s", name, installed_dir)

    cmake_build_dir = output_root / name / "cmake_build"
    cmake_build_dir.mkdir(parents=True, exist_ok=True)

    cmake_cmd = [
        "cmake", str(sde_path / "p4studio"),
        f"-DCMAKE_INSTALL_PREFIX={sde_install_path}",
        f"-DCMAKE_MODULE_PATH={sde_path}/cmake",
        f"-DP4_NAME={name}",
        f"-DP4_PATH={Path(p4_file).resolve()}",
    ]
    if extra_args:
        # Joined into one string so cmake sees a single P4PPFLAGS value; bf-p4c's
        # preprocessor accepts the space-separated "-I <dir>" / "-D<def>" form.
        p4ppflags = " ".join(extra_args)
        cmake_cmd.append(f"-DP4PPFLAGS={p4ppflags}")
        log.info("[%s] cmake P4PPFLAGS=%s", name, p4ppflags)

    _run(
        cmake_cmd,
        cwd=cmake_build_dir,
        log_path=cmake_build_dir / "cmake.log",
    )
    _run(["make", "install"], cwd=cmake_build_dir,
         log_path=cmake_build_dir / "make.log", timeout=3600)

    if not installed_dir.exists():
        raise TofinoStepError(
            f"cmake build completed but installed dir not found: {installed_dir}")
    return installed_dir


# --------------------------------------------------------------------------- #
# SDE process lifecycle                                                       #
# --------------------------------------------------------------------------- #

def _env_timeout(name: str, default: float) -> float:
    """Read a startup timeout from the environment, falling back to @p default."""
    raw = os.environ.get(name)
    if not raw:
        return default
    try:
        val = float(raw)
    except ValueError:
        log.warning("ignoring non-numeric %s=%r; using %.0fs", name, raw, default)
        return default
    if val <= 0:
        log.warning("ignoring non-positive %s=%r; using %.0fs", name, raw, default)
        return default
    return val


# How long to wait for each SDE process to start listening.
#
# These are per-RESTART, not per-run: under --reset-mode restart the differential replays the legit
# and the attack run from a pristine device, so _make_pristine() bounces tofino_model + bf_switchd
# TWICE per test case. The second bounce follows immediately on the first teardown, which is exactly
# when startup is slowest -- a too-short budget therefore shows up asymmetrically, as a legit run
# that passes followed by an attack run that errors out. 60s was too tight on a loaded VM.
#
# Override per run, e.g. SDE_SWITCHD_TIMEOUT=600 for a slow or heavily loaded machine.
SDE_MODEL_TIMEOUT = _env_timeout("SDE_MODEL_TIMEOUT", 120.0)
SDE_SWITCHD_TIMEOUT = _env_timeout("SDE_SWITCHD_TIMEOUT", 300.0)


def _wait_for_port(host: str, port: int, timeout: float = 60.0,
                   interval: float = 0.5, proc: Optional[subprocess.Popen] = None,
                   log_path: Optional[Path] = None, what: str = "") -> None:
    """Block until host:port accepts TCP connections or timeout elapses.

    Mirrors Bmv2Process._wait_for_grpc() in tampering.py.
    """
    deadline = time.time() + timeout
    started = time.time()
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            raise TofinoStepError(
                f"SDE process exited before port {port} came up (rc={proc.returncode})"
                f"{_log_tail(log_path)}")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            if s.connect_ex((host, port)) == 0:
                waited = time.time() - started
                # Surfaced because the useful signal is how close a PASSING start ran to the
                # budget: a run sitting at 80% of it is one busy moment away from erroring out.
                if waited > 0.5 * timeout:
                    log.warning("%s port %d took %.0fs of a %.0fs budget; consider raising %s",
                                what or "SDE", port, waited, timeout,
                                "SDE_SWITCHD_TIMEOUT" if port == DEFAULT_BFRT_GRPC_PORT
                                else "SDE_MODEL_TIMEOUT")
                return
        time.sleep(interval)
    raise TofinoStepError(
        f"{what or 'SDE'} port {port} did not come up within {timeout:.0f}s. If the process is "
        f"merely slow rather than broken, raise "
        f"{'SDE_SWITCHD_TIMEOUT' if port == DEFAULT_BFRT_GRPC_PORT else 'SDE_MODEL_TIMEOUT'} "
        f"(seconds).{_log_tail(log_path)}")


def _log_tail(log_path: Optional[Path], lines: int = 15) -> str:
    """Last few lines of an SDE log, for a timeout message. Without this the caller cannot tell a
    slow start from a crashed one, and both look identical in the CSV."""
    if log_path is None:
        return ""
    try:
        tail = log_path.read_text(errors="replace").splitlines()[-lines:]
    except Exception:
        return ""
    if not tail:
        return ""
    body = "\n    ".join(tail)
    return f"\n  --- tail of {log_path} ---\n    {body}"


def _sde_sudo() -> List[str]:
    """Prefix for killing SDE processes. They are usually launched as root by run_*.sh, so the
    harness (running as a normal user) needs sudo to signal them. No-op when already root.
    Uses `-n` (non-interactive) so it fails fast rather than hanging on a password prompt; the
    testbed already relies on passwordless sudo for tcpdump / simple_switch_grpc."""
    return [] if os.geteuid() == 0 else ["sudo", "-n"]


class SdeManagedProcess:
    """Base class for SDE background processes (tofino_model, bf_switchd)."""

    # Subclasses set this to the actual binary name for pkill fallback.
    # run_tofino_model.sh forks the binary into a new session and exits, so
    # killpg on the shell-script PID misses it; pkill by name catches it.
    pkill_name: Optional[str] = None

    def __init__(self, p4_name: str, sde: str, log_path: Path) -> None:
        self.p4_name = p4_name
        self.sde = sde
        self.log_path = log_path
        self.proc: Optional[subprocess.Popen] = None

    def _build_cmd(self) -> List[str]:
        raise NotImplementedError

    def start(self) -> None:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        cmd = self._build_cmd()
        log.info("starting SDE process: %s", " ".join(cmd))
        log_fd = self.log_path.open("w")
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL,
            stdout=log_fd, stderr=subprocess.STDOUT,
            preexec_fn=os.setsid,
        )

    def wait_ready(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        # Graceful TERM to the launcher's process group (works while the shell wrapper is alive
        # and same-user). May EPERM for the real binary if it re-sessions / runs as root — that's
        # fine, the sudo force-kill below handles it.
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), 15)  # SIGTERM to process group
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            except (ProcessLookupError, OSError):
                pass
        self.proc = None
        # Force-kill the real binary by name. run_tofino_model.sh / run_switchd.sh launch it
        # (typically as ROOT, for DMA/port access) into a new session, so killpg on the wrapper
        # misses it AND a non-sudo pkill gets EPERM — the process then survives SIGKILL and keeps
        # its ports, causing "bind: Address already in use" on the next start(). Kill with sudo.
        if self.pkill_name:
            self._force_pkill()
            self._wait_until_dead()

    def _force_pkill(self) -> None:
        try:
            subprocess.run(
                _sde_sudo() + ["pkill", "-KILL", "-f", self.pkill_name],
                check=False, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10,
            )
        except Exception:
            pass

    def _wait_until_dead(self, timeout: float = 20.0) -> None:
        """Block until no process matches ``pkill_name`` (its ports are then released),
        re-issuing the sudo kill each poll in case the first signal raced a slow/forked child."""
        if not self.pkill_name:
            return
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = subprocess.run(["pgrep", "-f", self.pkill_name],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if r.returncode != 0:   # pgrep: no process matched → fully dead, ports freed
                return
            self._force_pkill()
            time.sleep(0.3)
        log.warning("%s still running %ss after sudo kill; ports may still be busy "
                    "(is passwordless sudo available?)", self.pkill_name, timeout)


class TofinoModelProcess(SdeManagedProcess):
    """Manages run_tofino_model.sh for a single P4 program."""

    pkill_name = "tofino-model"

    def __init__(self, p4_name: str, sde: str, arch: str, log_path: Path,
                 ports_json: Path = _PORTS_JSON_PATH) -> None:
        super().__init__(p4_name, sde, log_path)
        self.arch = arch
        self.ports_json = ports_json

    def _build_cmd(self) -> List[str]:
        arch_str = "tofino2" if self.arch in ("t2na", "tofino2") else "tofino"
        return [f"{self.sde}/run_tofino_model.sh", "-p", self.p4_name,
                "--arch", arch_str, "-f", str(self.ports_json)]

    def wait_ready(self) -> None:
        time.sleep(2)
        # Tofino model exposes a thrift-like management port at 9090 before
        # the full gRPC stack is up — poll that as an early-ready signal.
        _wait_for_port(BFRT_GRPC_HOST, 9090, timeout=SDE_MODEL_TIMEOUT, proc=self.proc,
                       log_path=self.log_path, what="tofino_model")


class BfSwitchdProcess(SdeManagedProcess):
    """Manages run_switchd.sh for a single P4 program."""

    pkill_name = "bf_switchd"

    def _build_cmd(self) -> List[str]:
        return [f"{self.sde}/run_switchd.sh", "-p", self.p4_name]

    def wait_ready(self) -> None:
        _wait_for_port(BFRT_GRPC_HOST, DEFAULT_BFRT_GRPC_PORT,
                       timeout=SDE_SWITCHD_TIMEOUT, proc=self.proc,
                       log_path=self.log_path, what="bf_switchd")


# --------------------------------------------------------------------------- #
# Per-target program spec                                                     #
# --------------------------------------------------------------------------- #

@dataclass
class TofinoTarget:
    name: str
    p4_file: str
    arch: str = "tna"
    extra_args: List[str] = field(default_factory=list)


# --------------------------------------------------------------------------- #
# Pipeline entry points                                                       #
# --------------------------------------------------------------------------- #

def do_build(spec: TofinoTarget, args) -> Path:
    out_root = Path(args.output_root).expanduser()
    build_dir = out_root / spec.name / "build"
    p4c_bin = Path(getattr(args, "p4c_bin", None) or DEFAULT_P4C_BIN)
    return run_p4c(spec.name, spec.p4_file, spec.extra_args, build_dir,
                   arch=spec.arch, p4c_bin=p4c_bin,
                   force=getattr(args, "force_compile", False))


def do_p4symbex(spec: TofinoTarget, args) -> List[Path]:
    out_root = Path(args.output_root).expanduser()
    txtpb_dir = out_root / spec.name / "bfrt"
    p4symbex_bin = Path(getattr(args, "p4symbex_bin", None) or DEFAULT_P4SYMBEX_BIN)
    return run_p4symbex(
        spec.name, spec.p4_file, txtpb_dir,
        arch=spec.arch,
        max_tests=args.max_tests,
        tamper_value=args.tamper_value,
        skip=args.skip_p4symbex,
        p4symbex_bin=p4symbex_bin,
        extra_args=spec.extra_args,
    )


def do_testing(spec, build_dir: Path, txtpb_files: List[Path],
               args, csv_writer, progress_cb,
               manage_procs: bool = False) -> Tuple[int, int, int]:
    """Replay .txtpb tampering cases against tofino_model via BF-Runtime gRPC.

    When ``manage_procs=True`` the driver starts run_tofino_model.sh and
    run_switchd.sh, waits for the gRPC port to come up, runs tests, then
    stops both processes. Set ``manage_procs=False`` (default) when they
    are already running externally.

    Returns (ok, fail, err) protobuf-granularity counters.
    """
    from .bfrt_grpc_client import BfRtClient
    from .packet_tester import PacketTester, parse_case

    p4_name = getattr(spec, "p4_name", None) or spec.name
    arch = getattr(args, "arch", "tna")
    capture_dir = Path(args.output_root).expanduser() / spec.name / "pcap"
    capture_dir.mkdir(parents=True, exist_ok=True)
    log_dir = Path(args.output_root).expanduser() / spec.name

    # Ports referenced by the cases — both injected (in_port) and expected egress (exp_port).
    # A program may hardcode an egress port outside the standard 0-31 range (e.g. ACC-Turbo →
    # port 140); provision a veth + ports.json entry for those so the forward is observable.
    used_ports: set = set()
    for _tx in txtpb_files:
        try:
            _c = parse_case(_tx)
            for _ph in _c.phases:
                # Negative ports are p4symbex's "dropped / no egress" sentinel — not a real
                # device port (the model rejects them in PortToVeth); nothing to monitor.
                if _ph.in_port >= 0:
                    used_ports.add(_ph.in_port)
                if _ph.exp_port is not None and _ph.exp_port >= 0:
                    used_ports.add(_ph.exp_port)
        except Exception:
            pass
    ports_json_path, host_iface_map = prepare_ports_json(used_ports, log_dir)
    # Port→veth map covering only the ports used in this batch (avoids a tcpdump on every one of
    # the mapped veths). Derived from the (possibly extended) ports.json so out-of-range and
    # non-2N ports (e.g. CPU port 64 → veth250) map to the right host interface.
    port_to_iface = {p: host_iface_map[p]
                     for p in sorted(used_ports)
                     if p in host_iface_map} or {p: f"veth{2 * p}" for p in range(NUM_PORTS)}

    model_proc: Optional[TofinoModelProcess] = None
    switchd_proc: Optional[BfSwitchdProcess] = None

    if manage_procs:
        sde = os.environ.get("SDE")
        if not sde:
            csv_writer(spec.name, "", "ERROR", "$SDE not set; cannot start tofino_model")
            return 0, 0, len(txtpb_files)
        model_proc = TofinoModelProcess(
            p4_name=p4_name, sde=sde, arch=arch,
            log_path=log_dir / "tofino_model.log",
            ports_json=ports_json_path)
        switchd_proc = BfSwitchdProcess(
            p4_name=p4_name, sde=sde,
            log_path=log_dir / "bf_switchd.log")
        try:
            model_proc.start()
            switchd_proc.start()
            model_proc.wait_ready()
            switchd_proc.wait_ready()
        except TofinoStepError as ex:
            csv_writer(spec.name, "", "ERROR", f"SDE startup: {ex}")
            if switchd_proc:
                switchd_proc.stop()
            if model_proc:
                model_proc.stop()
            return 0, 0, len(txtpb_files)

    # Clean-state strategy between cases: 'registers' clears tables + zeroes register cells via
    # the control plane; 'restart' bounces tofino_model + bf_switchd for a pristine state (no
    # register-clear message burst). 'restart' needs the harness to own the processes.
    reset_mode = getattr(args, "reset_mode", "registers")
    restart_mode = reset_mode == "restart"
    if restart_mode and not manage_procs:
        log.warning("[%s] --reset-mode restart requires --sde-manage-procs; "
                    "falling back to register clear", spec.name)
        restart_mode = False

    def _connect():
        client = BfRtClient(BFRT_GRPC_ADDR, device_id=0, client_id=0, p4_name=p4_name)
        tester = PacketTester(client=client,
                              phase_timeout=args.phase_timeout,
                              capture_dir=capture_dir,
                              port_to_iface=port_to_iface,
                              strong_verify=getattr(args, "strong_verify", False))
        return client, tester

    def _restart_procs():
        # Bounce both SDE processes; a freshly (re)loaded pipeline has empty control-plane tables
        # and registers back at their P4-declared init values. stop() blocks until each binary is
        # actually dead (its listening sockets released), so the start()s below don't hit
        # "bind: Address already in use".
        switchd_proc.stop()
        model_proc.stop()
        model_proc.start()
        switchd_proc.start()
        model_proc.wait_ready()
        switchd_proc.wait_ready()

    ok_count = fail_count = err_count = 0
    try:
        try:
            client, tester = _connect()
        except Exception as ex:
            csv_writer(spec.name, "", "ERROR", f"bfrt-grpc connect: {ex}")
            return 0, 0, len(txtpb_files)
        # Differential oracle: replay each txtpb twice from a pristine state — legit (Phase 1 ->
        # Phase 3) vs attack (Phase 1 -> Phase 2 -> Phase 3) — and flag VULNERABLE iff the Phase-3
        # outputs differ (the only difference between the runs is the attacker's Phase-2 write).
        pristine = [True]  # the initial _connect() yields a clean switch; box for the closure

        def _make_pristine():
            nonlocal client, tester
            if pristine[0]:
                return
            if restart_mode:
                try:
                    client.close()
                except Exception:
                    pass
                _restart_procs()
                client, tester = _connect()
            else:
                client.clear_all_tables()
                client.clear_all_registers()
            pristine[0] = True

        try:
            total = len(txtpb_files)
            for idx, tx in enumerate(txtpb_files, 1):
                progress_cb(idx - 1, total, tx.name)
                try:
                    case = parse_case(tx)
                    # Indices are marker-derived: the legit run replays the non-tamper packets
                    # (Phase 1 + Phase 3); the attack run replays all, including the one-or-more
                    # tamper_only Phase-2 packets (accumulation sends the same packet k times).
                    legit_idx = case.legit_indices
                    attack_idx = case.attack_indices
                    # Verify the register after the LAST Phase-2 packet (before Phase 3 overwrites it).
                    tamper_idx = case.tamper_indices
                    verify_after = tamper_idx[-1] if tamper_idx else 1
                    # Legit run: Phase 1 -> Phase 3 (no attacker).
                    _make_pristine()
                    legit, _ = tester.replay(case, legit_idx, label=f"{tx.stem}_legit")
                    pristine[0] = False
                    # Attack run: Phase 1 -> Phase 2(xk) -> Phase 3 (verify the register after Phase 2).
                    _make_pristine()
                    attack, reg_diag = tester.replay(case, attack_idx, label=f"{tx.stem}_attack",
                                                     verify_regs_after=verify_after)
                    pristine[0] = False
                    ok, reason = tester.compare_runs(legit, attack)
                    if reg_diag:
                        reason = f"{reason} [Phase-2 reg: {reg_diag}]"
                except Exception as ex:
                    csv_writer(spec.name, tx.name, "ERROR", f"replay: {ex}")
                    err_count += 1
                    progress_cb(idx, total, tx.name)
                    continue
                csv_writer(spec.name, tx.name, "OK" if ok else "FAIL", reason)
                if ok:
                    ok_count += 1
                else:
                    fail_count += 1
                progress_cb(idx, total, tx.name)
        finally:
            try:
                client.close()
            except Exception:
                pass
    finally:
        if manage_procs:
            if switchd_proc:
                switchd_proc.stop()
            if model_proc:
                model_proc.stop()
    return ok_count, fail_count, err_count


def run_tofino_pipeline(args, csv_emit, targets: List[TofinoTarget]) -> None:
    """Top-level entry point invoked by tampering.main() when --target tofino.

    This path is only used when the Pipeline class does NOT handle tofino (i.e.
    for ad-hoc single-target runs or legacy callers). The Pipeline class in
    tampering.py is the primary execution path for multi-target batch runs.
    """
    out_root = Path(args.output_root).expanduser()
    out_root.mkdir(parents=True, exist_ok=True)
    if args.stage in ("run", "all"):
        require_sde()
    manage_procs = getattr(args, "sde_manage_procs", False)
    use_cmake = getattr(args, "sde_cmake_build", False)

    for spec in targets:
        target_root = out_root / spec.name
        target_root.mkdir(parents=True, exist_ok=True)
        # gen stage: p4symbex only.
        if args.stage in ("gen", "all"):
            try:
                txtpb_files = do_p4symbex(spec, args)
            except TofinoStepError as e:
                log.error("[%s] p4symbex failed: %s", spec.name, e)
                csv_emit(spec.name, "", "ERROR", f"p4symbex: {e}")
                continue
            if not txtpb_files:
                csv_emit(spec.name, "", "ERROR", "p4symbex produced no .txtpb files")
                continue
            if args.stage == "gen":
                csv_emit(spec.name, "", "OK", f"generated {len(txtpb_files)} txtpb")
                continue
        # run stage: compile then test.
        if args.stage in ("run", "all"):
            try:
                if use_cmake:
                    # extra_args carries the program-list's -I/-D flags; without them a forced
                    # rebuild of an include-using program (SketchLib et al.) fails in cpp. The
                    # skip path masked this, so it only surfaces once a compile actually runs.
                    build_dir = run_sde_cmake_build(
                        spec.name, spec.p4_file, out_root, extra_args=spec.extra_args,
                        force=getattr(args, "force_compile", False))
                else:
                    build_dir = do_build(spec, args)
            except TofinoStepError as e:
                log.error("[%s] compile failed: %s", spec.name, e)
                csv_emit(spec.name, "", "ERROR", f"compile: {e}")
                continue
            # Find pre-existing .txtpb files if we didn't just generate them.
            if args.stage == "run":
                txtpb_dir = out_root / spec.name / "bfrt"
                txtpb_files = sorted(txtpb_dir.glob("*.txtpb"))
                if not txtpb_files:
                    csv_emit(spec.name, "", "ERROR",
                             f"no .txtpb files in {txtpb_dir} — run --stage gen first")
                    continue
            do_testing(spec, build_dir, txtpb_files, args, csv_emit,
                       progress_cb=lambda *_, **__: None,
                       manage_procs=manage_procs)
