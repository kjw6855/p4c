#!/usr/bin/env python3
"""Capture a program's post-setup control-plane state from a live tofino-model.

    capture.py <program> [--out FILE] [--work DIR]

Runs the program's original controller (scenario programs/*/*/<program>/scenario.py, else
live/<program>.py) against tofino-model + bf_switchd, dumps the pipeline over BF-Runtime before and
after it, and writes what the controller changed as a CpSnapshot (default
sp27/cp_snapshots/live/<program>.txtpb).

A scenario module defines:
    PORTS    [Port]: the device ports the controller uses, with their roles (plus one ATTACKER).
    SOURCE   repo-relative directory copied into the work dir before patching (optional).
    PATCHES  [(file, old, new, reason)]: exact-text edits applied to that copy, never to the repo
             ("{SRC}" in `new` is the copy's absolute path).
             Only environment patches (names, paths, hardware-only calls, runtime loops) and
             outright bug fixes; rule-producing logic stays as written.
    SETUP_ONLY_PATCHES  like PATCHES, but applied only by capture (e.g. stopping the controller
             after its setup); validate_live.py runs the controller without them.
    READY_MARKER  optional: the controller keeps running after its setup (a reactive loop) and
             prints this once set up; validate_live.py then runs it in the background.
    prepare(ctx)  optional, before the target starts (e.g. building the program).
    supplement(ctx)  optional: entities the controller set that the dump cannot read back.
    run(ctx)      the controller step; raises on failure.
    STEPS    optional {name: packages.ControllerStep}: controller actions after the setup (a
             runtime update: a digest-triggering packet, a CLI line, one timer tick), and
    SCENARIOS  {scenario: [step names]}: the orders in which they run.

    capture.py <program> --sequence SCENARIO [--out FILE] writes a CpSnapshotSequence instead:
    `init` (what the setup changed, as above) and, per step, the updates it made. Every prefix of
    the scenario runs on a fresh target (setup, steps 1..j, then the controller is stopped and the
    target dumped), because a running BfRt controller keeps other clients from reading.
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import pickle
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

SDE = "/root/jiwon/Workspace/open-p4studio"
SDE_INSTALL = f"{SDE}/install"
SITE = f"{SDE_INSTALL}/lib/python3.12/site-packages"
os.environ.update(SDE=SDE, SDE_INSTALL=SDE_INSTALL, HOME="/root/jiwon")
os.environ["PATH"] = f"{SDE_INSTALL}/bin:" + os.environ["PATH"]
os.environ.setdefault("BFRT_SUBSCRIBE_TRIES", "30")

from cp_snapshot import dump_bfrt, packages, snapshot as snap  # noqa: E402
from tofino import tofino_driver as td  # noqa: E402
from tofino.bfrt_grpc_client import BfRtClient  # noqa: E402

REPOS = Path("/root/jiwon/Workspace-remote/top_tier_repo")
SNAPS = Path("/root/jiwon/Workspace-remote/sp27/cp_snapshots")
WORK = Path("/root/jiwon/Workspace-remote/sp27/cp_live")
PROGRAM_LIST = Path("/root/jiwon/Workspace-remote/usenix27/program_list_151_no_oom.txt")

log = logging.getLogger("capture")


# What run_p4_tests.sh would hand PTF: p4testutils reads these at import time.
PTF_PRELUDE = """
import ptf.testutils
ptf.testutils.TEST_PARAMS = {"arch": "tofino", "target": "asic-model", "num_pipes": "4"}
"""


def bfrt_name(program: str) -> str:
    """The name bfrt_python gives the program (bfrtcli.validate_program_name)."""
    if program.isidentifier():
        return program
    import re
    name = re.sub(r"[^\w]", "_", program, flags=re.ASCII)
    return name if program[:3].upper() == "P4_" else "p4_" + name


def load_scenario(program: str):
    return packages.load_module(packages.scenario_path(program), "live")


class Context:
    """What a scenario's prepare()/run() may use."""

    def __init__(self, program: str, work: Path) -> None:
        self.program = program
        self.work = work
        self.src: Optional[Path] = None
        self.notes: List[str] = []
        self.site = SITE
        self.env: Dict[str, str] = dict(os.environ, PYTHONPATH=f"{SITE}:{SITE}/tofino")
        # When set, commands run in the background until they print this (see READY_MARKER).
        self.ready_marker: Optional[str] = None
        self.background: List[subprocess.Popen] = []
        self.target = "tofino"

    def send(self, port: int, packet: bytes, count: int = 1) -> None:
        """Send `packet` into device port `port` (for steps a packet triggers, e.g. a digest)."""
        from tofino.packet_tester import _sudo_sendp
        # bmv2 binds veth{2p} and is fed through its peer; tofino-model owns veth{2p+1}.
        iface = f"veth{2 * port + 1}" if self.target == "bmv2" else f"veth{2 * port}"
        _sudo_sendp(packet, iface, count=count)

    def note(self, text: str) -> None:
        log.info("note: %s", text)
        self.notes.append(text)

    def _run(self, tag: str, argv: Sequence[str], cwd: Optional[Path], env: Dict[str, str],
             timeout: int, stdin: Optional[str] = None,
             hold_stdin: bool = False) -> subprocess.CompletedProcess:
        """Runs argv, logging to <work>/<tag>.log. hold_stdin keeps an empty stdin open until
        the process exits (bfshell quits as soon as its stdin closes)."""
        logf = self.work / f"{tag}.log"
        log.info("%s: %s (cwd %s)", tag, " ".join(map(str, argv)), cwd)
        with logf.open("a") as fd:
            fd.write(f"$ {' '.join(map(str, argv))}\n")
            fd.flush()
            if self.ready_marker:
                return self._start_until_ready(argv, cwd, env, timeout, fd, logf)
            piped = stdin is not None or hold_stdin
            proc = subprocess.Popen(list(map(str, argv)), cwd=cwd, env=env, stdout=fd,
                                    stderr=subprocess.STDOUT, text=True, start_new_session=True,
                                    stdin=subprocess.PIPE if piped else subprocess.DEVNULL)
            try:
                if hold_stdin:
                    proc.wait(timeout=timeout)
                    proc.stdin.close()
                else:
                    proc.communicate(stdin, timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, 9)
                proc.wait()
                raise RuntimeError(f"{tag} timed out after {timeout}s (log {logf})")
        return subprocess.CompletedProcess(argv, proc.returncode)

    def _start_until_ready(self, argv, cwd, env, timeout, fd, logf) -> subprocess.CompletedProcess:
        """Starts argv and returns once its log shows ready_marker, leaving it running (stopped by
        stop_background). Returns its exit code if it exits before that."""
        start = fd.tell()
        proc = subprocess.Popen(list(map(str, argv)), cwd=cwd, env=dict(env, PYTHONUNBUFFERED="1"),
                                stdout=fd, stderr=subprocess.STDOUT, text=True,
                                start_new_session=True, stdin=subprocess.DEVNULL)
        deadline = time.time() + timeout
        while True:
            with logf.open() as rd:
                rd.seek(start)
                if self.ready_marker in rd.read():
                    self.background.append(proc)
                    log.info("%s is set up (%r) and keeps running", argv[-1], self.ready_marker)
                    return subprocess.CompletedProcess(argv, 0)
            if proc.poll() is not None:
                return subprocess.CompletedProcess(argv, proc.returncode)
            if time.time() > deadline:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                raise RuntimeError(f"no {self.ready_marker!r} after {timeout}s (log {logf})")
            time.sleep(0.2)

    def stop_background(self) -> None:
        for proc in self.background:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
        self.background.clear()

    def python(self, argv: Sequence[str], cwd: Path, env: Optional[Dict[str, str]] = None,
               timeout: int = 1800, tag: str = "controller") -> int:
        e = dict(self.env, **(env or {}))
        return self._run(tag, [sys.executable, *argv], cwd, e, timeout).returncode

    def bfshell(self, script: Path, cwd: Optional[Path] = None, timeout: int = 1800,
                tag: str = "controller") -> str:
        """Runs a bfrt_python script in bfshell and returns its output. bfshell exits 0 even when
        the script raises, so callers check the returned text."""
        logf = self.work / f"{tag}.log"
        start = logf.stat().st_size if logf.exists() else 0
        # bfshell needs a terminal: run it under script(1).
        cmd = f"{SDE}/run_bfshell.sh --no-status-srv -b {script}"
        self._run(tag, ["script", "-q", "-e", "-c", cmd, "/dev/null"], cwd or script.parent,
                  self.env, timeout, hold_stdin=True)
        with logf.open() as fd:
            fd.seek(start)
            return fd.read()

    def ptf(self, test_dir: Path, test: str, params: str = "", pythonpath: str = "",
            timeout: int = 1800, tag: str = "controller") -> int:
        argv = [f"{SDE}/run_p4_tests.sh", "-p", self.program, "-t", str(test_dir), "-s", test]
        if params:
            argv += ["--test-params", params]
        env = dict(self.env)
        if pythonpath:
            env["PYTHONPATH"] = f"{pythonpath}:{env['PYTHONPATH']}"
        return self._run(tag, argv, test_dir, env, timeout).returncode


def apply_patches(ctx: Context, patches) -> None:
    with (ctx.work / "patches.log").open("w") as fd:
        for rel, old, new, reason in patches:
            f = ctx.src / rel
            text = f.read_text()
            n = text.count(old)
            if n == 0:
                raise RuntimeError(f"patch target not found in {rel}: {old[:60]!r}")
            new = new.replace("{SRC}", str(ctx.src))  # the patched copy's absolute path
            f.write_text(text.replace(old, new))
            fd.write(f"{rel} (x{n}): {reason}\n  - {old!r}\n  + {new!r}\n")
            ctx.notes.append(f"patch {rel}: {reason}")


def dump_once(program: str, tag: str, client_id: int = 0):
    # Any client_id but 0 leaves digests to a running controller (which holds client 0).
    client = BfRtClient(td.BFRT_GRPC_ADDR, client_id=client_id, p4_name=program,
                        enable_learn=client_id == 0)
    try:
        t0 = time.time()
        entities, notes = dump_bfrt.dump(client)
    finally:
        client.close()
    log.info("%s: %d entities in %.0fs, %d notes", tag, len(entities), time.time() - t0,
             len(notes))
    return entities, notes


def program_p4(program: str) -> Path:
    for line in PROGRAM_LIST.read_text().splitlines():
        f = line.split()
        if f and f[0] == program:
            return Path(f[1].replace("~/", "/root/"))
    raise KeyError(f"{program} not in {PROGRAM_LIST}")


def _run_controller(scen, ctx: Context) -> int:
    try:
        scen.run(ctx)
        return 0
    except Exception as e:  # noqa: BLE001 - still dump what the controller got to install
        ctx.note(f"controller failed: {e}")
        return 1


class CustomSwitchd(td.BfSwitchdProcess):
    """A controller that embeds the switch driver itself (bf_switchd_lib_init), started in place
    of bf_switchd with run_switchd.sh's environment."""

    def __init__(self, argv: List[str], log_path: Path) -> None:
        super().__init__("", SDE, log_path)
        self.argv = argv
        self.pkill_name = Path(argv[0]).name

    def _build_cmd(self) -> List[str]:
        return ["env", f"SDE={SDE}", f"SDE_INSTALL={SDE_INSTALL}",
                f"LD_LIBRARY_PATH={SDE_INSTALL}/lib", *self.argv]


def _tofino_session(program: str, ctx: Context, ports_json: Path, tag: str, switchd, body):
    model = td.TofinoModelProcess(program, SDE, "tna", ctx.work / f"tofino_model{tag}.log",
                                  ports_json=ports_json)
    try:
        model.start()
        switchd.start()  # the model/switchd handshake is mutual: start both before waiting
        model.wait_ready()
        switchd.wait_ready()
        return body()
    finally:
        switchd.stop()
        model.stop()


def write_ports_json(path: Path, ports: List[int]) -> Path:
    """tofino-model port map: device port p on veth{2p+1}; the host side is veth{2p}."""
    path.write_text(json.dumps({"PortToVeth": [
        {"device_port": p, "veth1": 2 * p, "veth2": 2 * p + 1} for p in ports]}, indent=2))
    return path


def _tofino(program: str, scen, ctx: Context, ports: List[int]):
    work = ctx.work
    ports_json = write_ports_json(work / "ports.json", ports)
    stock = td.BfSwitchdProcess(program, SDE, work / "bf_switchd.log")
    if not hasattr(scen, "switchd_argv"):
        def body():
            before, _ = dump_once(program, "before")
            rc = _run_controller(scen, ctx)
            after, notes = dump_once(program, "after")
            return before, after, notes, rc
        return _tofino_session(program, ctx, ports_json, "", stock, body)
    # The controller is the switch process: "before" comes from a pristine stock bf_switchd, and
    # "after" from the controller's own process once its setup is done.
    before, _ = _tofino_session(program, ctx, ports_json, "", stock,
                                lambda: dump_once(program, "before"))
    custom = CustomSwitchd(scen.switchd_argv(ctx), work / "controller_switchd.log")

    def body():
        rc = _run_controller(scen, ctx)
        after, notes = dump_once(program, "after")
        return before, after, notes, rc
    return _tofino_session(program, ctx, ports_json, "_controller", custom, body)


def bmv2_switch(program: str, ctx: Context, ports: List[int]):
    """Compiles the program and returns (switch, p4info), with ctx.cli feeding that switch."""
    from cp_snapshot import bmv2_live
    js, info = bmv2_live.compile_program(program_p4(program), ctx.work)
    sw = bmv2_live.Switch(js, ports, ctx.work)

    def cli(commands: str, tag: str = "controller") -> str:
        text = sw.cli(commands)
        with (ctx.work / f"{tag}.log").open("a") as fd:
            fd.write(f"$ simple_switch_CLI <<{len(commands.splitlines())} lines>>\n{text}\n")
        return text

    ctx.cli = cli
    return sw, info


def _bmv2(program: str, scen, ctx: Context, ports: List[int]):
    from cp_snapshot import bmv2_live
    sw, info = bmv2_switch(program, ctx, ports)
    try:
        sw.start()
        before, _ = bmv2_live.to_entities(sw.dump(), info)
        rc = _run_controller(scen, ctx)
        after, notes = bmv2_live.to_entities(sw.dump(), info)
    finally:
        sw.stop()
    return before, after, notes, rc


def prepare_work(program: str, scen, work: Path, setup_only: bool = True) -> Context:
    """Fresh work dir: the patched controller copy (SETUP_ONLY_PATCHES too when setup_only) and
    the scenario's prepare step."""
    work.mkdir(parents=True, exist_ok=True)
    os.chdir(work)  # tofino-model and bf_switchd write their logs into the current directory
    for f in ("controller.log", "patches.log"):
        (work / f).unlink(missing_ok=True)
    ctx = Context(program, work)
    if getattr(scen, "SOURCE", None):
        ctx.src = work / "src"
        if ctx.src.exists():
            shutil.rmtree(ctx.src)
        shutil.copytree(REPOS / scen.SOURCE, ctx.src, symlinks=True)
        patches = list(getattr(scen, "PATCHES", []))
        if setup_only:
            patches += getattr(scen, "SETUP_ONLY_PATCHES", [])
        apply_patches(ctx, patches)
    if hasattr(scen, "prepare"):
        scen.prepare(ctx)
    return ctx


def make_veths(ports) -> List[int]:
    """Creates the veth{2p}/veth{2p+1} pairs missing for `ports`; returns the ports created."""
    created = [p for p in ports if not td._veth_exists(f"veth{2 * p}")]
    for p in created:
        td._ensure_veth_pair(2 * p, 2 * p + 1)
    return created


def drop_veths(created: List[int]) -> None:
    for p in created:
        subprocess.run(["ip", "link", "del", f"veth{2 * p}"], capture_output=True)


def capture(program: str, out: Path, work: Path) -> int:
    scen = load_scenario(program)
    target = getattr(scen, "TARGET", "tofino")
    ctx = prepare_work(program, scen, work)

    ports = [p.port for p in scen.PORTS]
    created = make_veths(ports)
    try:
        run = _bmv2 if target == "bmv2" else _tofino
        before, after, dump_notes, rc = run(program, scen, ctx, ports)
    finally:
        drop_veths(created)

    changed = snap.diff(before, after)
    if hasattr(scen, "supplement"):
        # State the controller set but the target's read API cannot show (see the scenario).
        extra = scen.supplement(ctx)
        changed += extra
        ctx.note(f"supplemented {len(extra)} entities the target does not read back")
    (work / "entities.pkl").write_bytes(pickle.dumps(
        {"before": before, "after": after, "changed": changed}))
    s = snap.Snapshot(program=program, target=target, ports=list(scen.PORTS),
                      entities=changed, snapshot_id="live",
                      notes=[f"live capture on {target}: {getattr(scen, 'DESCRIPTION', '')}"]
                      + ctx.notes + [f"dump: {n}" for n in dump_notes])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(snap.to_textproto(s))
    log.info("wrote %s: %d changed entities (rc=%d)", out, len(changed), rc)
    return rc


# --- snapshot sequences (--sequence) -----------------------------------------------------------

def _dump_unbound(program: str, tag: str, tries: int = 30):
    """Dump through BfRt client 1: the controller (client 0) may still be releasing its binding,
    which the server drops lazily."""
    for n in range(tries):
        try:
            return dump_once(program, tag, client_id=1)
        except Exception:  # noqa: BLE001
            if n == tries - 1:
                raise
            time.sleep(1)
    raise AssertionError("unreachable")


def _run_prefix(scen, ctx: Context, steps, settle: float) -> int:
    """The setup, then `steps` [(name, ControllerStep)]; the controller is stopped on return. A
    reactive controller (READY_MARKER) keeps running in the background while the steps run."""
    try:
        rc = _run_controller(scen, ctx)
        marker, ctx.ready_marker = ctx.ready_marker, None  # steps' own commands run to completion
        try:
            for name, st in steps:
                log.info("step %s: %s", name, st.trigger)
                with (ctx.work / "controller.log").open("a") as fd:
                    fd.write(f"\n===== step {name}: {st.trigger} =====\n")
                st.run(ctx)
                time.sleep(settle)
        finally:
            ctx.ready_marker = marker
        return rc
    finally:
        ctx.stop_background()


def _tofino_prefix(program: str, scen, ctx: Context, ports_json: Path, steps, settle: float,
                   pristine: bool):
    switchd = td.BfSwitchdProcess(program, SDE, ctx.work / "bf_switchd.log")

    def body():
        before = dump_once(program, "pristine")[0] if pristine else None
        rc = _run_prefix(scen, ctx, steps, settle)
        after, notes = _dump_unbound(program, f"after {len(steps)} step(s)")
        return before, after, notes, rc
    return _tofino_session(program, ctx, ports_json, f"_seq{len(steps)}", switchd, body)


def _bmv2_prefix(program: str, scen, ctx: Context, sw_info, steps, settle: float, pristine: bool):
    from cp_snapshot import bmv2_live
    sw, info = sw_info
    try:
        sw.start()
        before = bmv2_live.to_entities(sw.dump(), info)[0] if pristine else None
        rc = _run_prefix(scen, ctx, steps, settle)
        after, notes = bmv2_live.to_entities(sw.dump(), info)
    finally:
        sw.stop()
    return before, after, notes, rc


def _register_name(e: snap.Entity, p4info: Optional[dict]) -> str:
    """A register cell's register name (bmv2 cells carry their P4Info id)."""
    if e.table.startswith("register_id:") and p4info:
        rid = int(e.table.split(":", 1)[1])
        for r in p4info.get("registers", []):
            if r["preamble"]["id"] == rid:
                return r["preamble"]["name"]
    return e.table


def _suffix_match(name: str, wanted: str) -> bool:
    return name == wanted or name.endswith("." + wanted) or wanted.endswith("." + name)


def capture_sequence(program: str, scenario: str, out: Path, work: Path, settle: float = 1.0,
                     check_determinism: bool = False) -> int:
    scen = load_scenario(program)
    steps_by_name = getattr(scen, "STEPS", {})
    scenarios = getattr(scen, "SCENARIOS", {})
    if scenario not in scenarios:
        raise SystemExit(f"{program}: no scenario {scenario!r} (have {sorted(scenarios)})")
    steps = [(name, steps_by_name[name]) for name in scenarios[scenario]]
    target = getattr(scen, "TARGET", "tofino")
    # The full controller (no SETUP_ONLY_PATCHES): a reactive one must keep running for steps
    # that make it act (a digest, a timer tick).
    ctx = prepare_work(program, scen, work, setup_only=False)
    ctx.ready_marker = getattr(scen, "READY_MARKER", None)
    ctx.target = target
    ports = [p.port for p in scen.PORTS]
    created = make_veths(ports)
    p4info = None
    try:
        if target == "bmv2":
            sw_info = bmv2_switch(program, ctx, ports)
            p4info = sw_info[1]

            def prefix(j, pristine):
                return _bmv2_prefix(program, scen, ctx, sw_info, steps[:j], settle, pristine)
        else:
            ports_json = write_ports_json(work / "ports.json", ports)

            def prefix(j, pristine):
                return _tofino_prefix(program, scen, ctx, ports_json, steps[:j], settle, pristine)
        dumps, rcs, dump_notes = [], [], []
        pristine = None
        for j in range(len(steps) + 1):
            before, after, notes, rc = prefix(j, j == 0)
            if j == 0:
                pristine = before
            dumps.append(after)
            rcs.append(rc)
            dump_notes += notes
        if check_determinism:
            _, again, _, _ = prefix(0, False)
            drift = snap.updates(dumps[0], again)
            if drift:
                raise SystemExit(f"{program}: the setup is not deterministic: {len(drift)} "
                                 f"entities differ between two runs, e.g. {drift[0]}")
            log.info("determinism: two setup runs dump identically")
    finally:
        drop_veths(created)

    init = snap.diff(pristine, dumps[0])
    if hasattr(scen, "supplement"):
        init += scen.supplement(ctx)
    notes = [f"snapshot sequence on {target}: {getattr(scen, 'DESCRIPTION', '')}",
             f"scenario {scenario}: {' -> '.join(n for n, _ in steps)}"] + ctx.notes
    seq_steps = []
    for j, (name, st) in enumerate(steps, 1):
        kept, dropped = [], 0
        for op, e in snap.updates(dumps[j - 1], dumps[j]):
            if e.is_register and not any(_suffix_match(_register_name(e, p4info), r)
                                         for r in st.registers):
                dropped += 1  # a data-plane write by a packet the step sent
                continue
            kept.append((op, e))
        notes.append(f"step {name} (rc={rcs[j]}): {len(kept)} updates, {dropped} data-plane "
                     f"register changes dropped")
        seq_steps.append(snap.Step(name, st.trigger, kept))
    s = snap.Snapshot(program=program, target=target, ports=list(scen.PORTS), entities=init,
                      snapshot_id="init")
    seq = snap.Sequence(init=s, scenario=scenario, steps=seq_steps,
                        notes=notes + [f"dump: {n}" for n in dict.fromkeys(dump_notes)])
    (work / f"sequence_{scenario}.pkl").write_bytes(pickle.dumps(
        {"pristine": pristine, "dumps": dumps}))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(snap.sequence_to_textproto(seq))
    log.info("wrote %s: init %d entities, steps %s", out, len(init),
             [len(st.updates) for st in seq_steps])
    return max(rcs)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--work", type=Path)
    ap.add_argument("--sequence", metavar="SCENARIO",
                    help="capture init plus one diff per controller step of SCENARIO")
    ap.add_argument("--settle", type=float, default=1.0,
                    help="seconds to wait after each step (--sequence)")
    ap.add_argument("--check-determinism", action="store_true",
                    help="run the setup twice and fail if the dumps differ (--sequence)")
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    work = (a.work or WORK / a.program).resolve()
    if a.sequence:
        out = (a.out or work / "snapshot" / "seq" / f"{a.sequence}.txtpb").resolve()
        return capture_sequence(a.program, a.sequence, out, work, a.settle, a.check_determinism)
    out = (a.out or SNAPS / "live" / f"{a.program}.txtpb").resolve()
    return capture(a.program, out, work)


if __name__ == "__main__":
    sys.exit(main())
