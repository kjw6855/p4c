#!/usr/bin/env python3
"""Capture a program's post-setup control-plane state from a live tofino-model.

    capture.py <program> [--out FILE] [--work DIR]

Runs the program's original controller (scenario live/<program>.py) against tofino-model +
bf_switchd, dumps the pipeline over BF-Runtime before and after it, and writes what the controller
changed as a CpSnapshot (default sp27/cp_snapshots/live/<program>.txtpb).

A scenario module defines:
    PORTS    [Port]: the device ports the controller uses, with their roles (plus one ATTACKER).
    SOURCE   repo-relative directory copied into the work dir before patching (optional).
    PATCHES  [(file, old, new, reason)]: exact-text edits applied to that copy, never to the repo
             ("{SRC}" in `new` is the copy's absolute path).
             Only environment patches (names, paths, hardware-only calls, runtime loops) and
             outright bug fixes; rule-producing logic stays as written.
    prepare(ctx)  optional, before the target starts (e.g. building the program).
    supplement(ctx)  optional: entities the controller set that the dump cannot read back.
    run(ctx)      the controller step; raises on failure.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import logging
import os
import pickle
import shutil
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

from cp_snapshot import dump_bfrt, snapshot as snap  # noqa: E402
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
    path = HERE / "live" / f"{program}.py"
    spec = importlib.util.spec_from_file_location(f"live_{abs(hash(program))}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Context:
    """What a scenario's prepare()/run() may use."""

    def __init__(self, program: str, work: Path) -> None:
        self.program = program
        self.work = work
        self.src: Optional[Path] = None
        self.notes: List[str] = []
        self.site = SITE
        self.env: Dict[str, str] = dict(os.environ, PYTHONPATH=f"{SITE}:{SITE}/tofino")

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


def dump_once(program: str, tag: str):
    client = BfRtClient(td.BFRT_GRPC_ADDR, client_id=0, p4_name=program)
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


def _tofino(program: str, scen, ctx: Context, ports: List[int]):
    work = ctx.work
    ports_json = work / "ports.json"
    ports_json.write_text(json.dumps({"PortToVeth": [
        {"device_port": p, "veth1": 2 * p, "veth2": 2 * p + 1} for p in ports]}, indent=2))
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


def _bmv2(program: str, scen, ctx: Context, ports: List[int]):
    from cp_snapshot import bmv2_live
    js, info = bmv2_live.compile_program(program_p4(program), ctx.work)
    sw = bmv2_live.Switch(js, ports, ctx.work)

    def cli(commands: str, tag: str = "controller") -> str:
        text = sw.cli(commands)
        with (ctx.work / f"{tag}.log").open("a") as fd:
            fd.write(f"$ simple_switch_CLI <<{len(commands.splitlines())} lines>>\n{text}\n")
        return text

    ctx.cli = cli
    try:
        sw.start()
        before, _ = bmv2_live.to_entities(sw.dump(), info)
        rc = _run_controller(scen, ctx)
        after, notes = bmv2_live.to_entities(sw.dump(), info)
    finally:
        sw.stop()
    return before, after, notes, rc


def capture(program: str, out: Path, work: Path) -> int:
    scen = load_scenario(program)
    target = getattr(scen, "TARGET", "tofino")
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
        apply_patches(ctx, getattr(scen, "PATCHES", []))
    if hasattr(scen, "prepare"):
        scen.prepare(ctx)

    ports = [p.port for p in scen.PORTS]
    created = [p for p in ports if not td._veth_exists(f"veth{2 * p}")]
    for p in created:
        td._ensure_veth_pair(2 * p, 2 * p + 1)
    try:
        run = _bmv2 if target == "bmv2" else _tofino
        before, after, dump_notes, rc = run(program, scen, ctx, ports)
    finally:
        for p in created:
            subprocess.run(["ip", "link", "del", f"veth{2 * p}"], capture_output=True)

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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--work", type=Path)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    out = (a.out or SNAPS / "live" / f"{a.program}.txtpb").resolve()
    work = (a.work or WORK / a.program).resolve()
    return capture(a.program, out, work)


if __name__ == "__main__":
    sys.exit(main())
