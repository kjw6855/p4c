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

NUM_PORTS = 32
# Even-numbered veth is the host/PTF side; odd-numbered veth is the model side.
# This matches the standard SDE veth_setup.sh pairing convention.
PORT_TO_HOST_IFACE = {n: f"veth{2 * n}" for n in range(NUM_PORTS)}

# ports.json passed to run_tofino_model.sh via -f, mapping device ports 0-31
# to their veth pairs (port N → veth{2N}/veth{2N+1}).
_PORTS_JSON_PATH = Path(__file__).parent / "ports.json"

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
            p4_version: str = "p4-16") -> Path:
    """Compile a TNA .p4 program. Returns the directory of the produced
    artifacts (context.json, bfrt.json, tofino.bin live here)."""
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

    if _has_artifacts(build_dir):
        log.info("[%s] tofino artifacts present, skipping p4c", name)
        return build_dir
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
                 extra_args: List[str] = []) -> List[Path]:
    """Run p4symbex with the BFRT backend to emit .txtpb tampering cases."""
    if skip and txtpb_dir.exists():
        files = sorted(txtpb_dir.glob("*.txtpb"))
        if files:
            log.info("[%s] reusing %d existing txtpb files", name, len(files))
            return files
    if txtpb_dir.exists():
        for f in txtpb_dir.glob("*.txtpb"):
            f.unlink()
    txtpb_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(p4symbex_bin),
        "--target", "tofino",
        "--arch", arch,
        "--std", p4_version,
        _target_define(arch),
        *P4C_INCLUDE_FLAGS,
        "--test-backend", "BFRT",
        "--packet-size-range", "0:9600",
        "--track-coverage", "STATEMENTS",
        "--max-tests", str(max_tests),
        "--max-port", str(NUM_PORTS),
        "--out-dir", str(txtpb_dir),
        "--state-dep",
        "--path-selection", path_selection,
        *extra_args,
        p4_file,
        "--state-tamper-value", tamper_value,
    ]
    _run(cmd, log_path=txtpb_dir.parent / "p4symbex.log", timeout=1800)
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

def run_sde_cmake_build(name: str, p4_file: str, output_root: Path) -> Path:
    """Build a TNA program via cmake + make install using the SDE p4studio.

    The SDE's run_tofino_model.sh / run_switchd.sh expect the compiled program
    to live under $SDE_INSTALL/share/tofinopd/<name>/. This function achieves
    that via the standard 'cmake $SDE/p4studio … && make install' workflow.
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

    # Smart-skip: already installed with a .bin artifact.
    if installed_dir.exists() and list(installed_dir.rglob("*.bin")):
        log.info("[%s] cmake artifacts already installed at %s, skipping", name, installed_dir)
        return installed_dir

    cmake_build_dir = output_root / name / "cmake_build"
    cmake_build_dir.mkdir(parents=True, exist_ok=True)

    _run(
        ["cmake", str(sde_path / "p4studio"),
         f"-DCMAKE_INSTALL_PREFIX={sde_install_path}",
         f"-DCMAKE_MODULE_PATH={sde_path}/cmake",
         f"-DP4_NAME={name}",
         f"-DP4_PATH={Path(p4_file).resolve()}"],
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

def _wait_for_port(host: str, port: int, timeout: float = 60.0,
                   interval: float = 0.5, proc: Optional[subprocess.Popen] = None) -> None:
    """Block until host:port accepts TCP connections or timeout elapses.

    Mirrors Bmv2Process._wait_for_grpc() in tampering.py.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            raise TofinoStepError(
                f"SDE process exited before port {port} came up (rc={proc.returncode})")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            if s.connect_ex((host, port)) == 0:
                return
        time.sleep(interval)
    raise TofinoStepError(f"port {port} did not come up within {timeout:.0f}s")


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
        # Kill via process group first (works when the shell script is still alive).
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), 15)  # SIGTERM to process group
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(os.getpgid(self.proc.pid), 9)  # SIGKILL
                    self.proc.wait(timeout=3)
            except ProcessLookupError:
                pass
        self.proc = None
        # Belt-and-suspenders: the launcher script (run_tofino_model.sh /
        # run_switchd.sh) often forks the real binary into a new session and
        # exits, making killpg above a no-op for the actual process.
        if self.pkill_name:
            try:
                subprocess.run(
                    ["pkill", "-KILL", "-f", self.pkill_name],
                    check=False, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    timeout=5,
                )
            except Exception:
                pass


class TofinoModelProcess(SdeManagedProcess):
    """Manages run_tofino_model.sh for a single P4 program."""

    pkill_name = "tofino-model"

    def __init__(self, p4_name: str, sde: str, arch: str, log_path: Path) -> None:
        super().__init__(p4_name, sde, log_path)
        self.arch = arch

    def _build_cmd(self) -> List[str]:
        arch_str = "tofino2" if self.arch in ("t2na", "tofino2") else "tofino"
        return [f"{self.sde}/run_tofino_model.sh", "-p", self.p4_name,
                "--arch", arch_str, "-f", str(_PORTS_JSON_PATH)]

    def wait_ready(self) -> None:
        time.sleep(2)
        # Tofino model exposes a thrift-like management port at 9090 before
        # the full gRPC stack is up — poll that as an early-ready signal.
        _wait_for_port(BFRT_GRPC_HOST, 9090, timeout=30, proc=self.proc)


class BfSwitchdProcess(SdeManagedProcess):
    """Manages run_switchd.sh for a single P4 program."""

    pkill_name = "bf_switchd"

    def _build_cmd(self) -> List[str]:
        return [f"{self.sde}/run_switchd.sh", "-p", self.p4_name]

    def wait_ready(self) -> None:
        _wait_for_port(BFRT_GRPC_HOST, DEFAULT_BFRT_GRPC_PORT,
                       timeout=60, proc=self.proc)


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
                   arch=spec.arch, p4c_bin=p4c_bin)


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

    model_proc: Optional[TofinoModelProcess] = None
    switchd_proc: Optional[BfSwitchdProcess] = None

    if manage_procs:
        sde = os.environ.get("SDE")
        if not sde:
            csv_writer(spec.name, "", "ERROR", "$SDE not set; cannot start tofino_model")
            return 0, 0, len(txtpb_files)
        model_proc = TofinoModelProcess(
            p4_name=p4_name, sde=sde, arch=arch,
            log_path=log_dir / "tofino_model.log")
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

    ok_count = fail_count = err_count = 0
    try:
        try:
            client = BfRtClient(BFRT_GRPC_ADDR, device_id=0, client_id=0,
                                p4_name=p4_name)
        except Exception as ex:
            csv_writer(spec.name, "", "ERROR", f"bfrt-grpc connect: {ex}")
            return 0, 0, len(txtpb_files)
        try:
            # Build a port→veth map covering only the ports used in this batch
            # to avoid starting a tcpdump on every one of the 32 mapped veths.
            used_ports: set = set()
            for _tx in txtpb_files:
                try:
                    _c = parse_case(_tx)
                    for _ph in _c.phases:
                        used_ports.add(_ph.in_port)
                        if _ph.exp_port is not None:
                            used_ports.add(_ph.exp_port)
                except Exception:
                    pass
            port_to_iface = {p: PORT_TO_HOST_IFACE[p]
                             for p in sorted(used_ports)
                             if p in PORT_TO_HOST_IFACE}
            if not port_to_iface:
                port_to_iface = dict(PORT_TO_HOST_IFACE)
            tester = PacketTester(client=client,
                                  phase_timeout=args.phase_timeout,
                                  capture_dir=capture_dir,
                                  port_to_iface=port_to_iface)
            total = len(txtpb_files)
            for idx, tx in enumerate(txtpb_files, 1):
                progress_cb(idx - 1, total, tx.name)
                try:
                    client.clear_all_tables()
                    client.clear_all_registers()
                    case = parse_case(tx)
                    ok, reason = tester.run(case, label=tx.stem)
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
            client.close()
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
                    build_dir = run_sde_cmake_build(spec.name, spec.p4_file, out_root)
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
