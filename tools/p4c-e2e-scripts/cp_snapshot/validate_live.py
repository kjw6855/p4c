#!/usr/bin/env python3
"""Validate cp-snapshot cases with the program's own controller in the loop (tofino and bmv2).

    validate_live.py <program> <cases_dir> [--out DIR] [--settle 1.0] [--phase-timeout 2.0]
                     [--baseline CSV] [--only SUBSTR ...]

The phase-0 replay (replay_campaign.py, replay_bmv2.sh) installs each case's snapshot block and
sends the packets with no controller running, the frozen-rules assumption p4symbex makes. Here
every run (legit P1 -> P3, attack P1 -> P2 -> P3) gets a fresh target configured by the scenario's
controller step, exactly as capture.py runs it but without SETUP_ONLY_PATCHES; a scenario with
READY_MARKER (a reactive controller) stays running in the background while the phases go out.
Nothing from the case is installed. After each phase the run sleeps --settle seconds, so rules a
reactive controller adds in response are in place for the next phase.

Writes to --out (default <work>/_validate/out):
  controller_replay.csv  category,case,status,reason,controller_rc,runtime_rows,baseline,agree
                         (OK = the attack reproduced; baseline/agree from --baseline, the phase-0
                         replay verdicts, any CSV whose rows hold "<case>.txtpb,<status>")
  drift.txt              the first run's post-setup state against capture.py's (entities.pkl)
  runtime/<case>_<run>.txt  table rows that changed during the phases (READY_MARKER scenarios)
"""
from __future__ import annotations

import argparse
import csv
import logging
import pickle
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot import capture, dump_bfrt, snapshot as snap  # noqa: E402
from cp_snapshot.compare import ident  # noqa: E402
from tofino import tofino_driver as td  # noqa: E402
from tofino.bfrt_grpc_client import BfRtClient  # noqa: E402
from tofino.packet_tester import PacketTester  # noqa: E402
from tofino.txtpb_parser import TamperCase, parse_tampering_txtpb  # noqa: E402

log = logging.getLogger("validate_live")


@dataclass
class Run:
    capture: Dict[int, List[bytes]]
    reg_diag: str
    controller_rc: int
    setup: Optional[List[snap.Entity]] = None
    runtime: List[str] = field(default_factory=list)


def run_indices(case: TamperCase) -> Tuple[List[int], List[int], List[int]]:
    """(legit, attack, tamper) phase indices. Cases without tamper markers (the bmv2 generator's)
    are three phases P1, P2, P3, as tampering.py replays them."""
    if not case.tamper_indices and len(case.phases) == 3:
        return [0, 2], [0, 1, 2], [1]
    return case.legit_indices, case.attack_indices, case.tamper_indices


def case_ports(case: TamperCase) -> set:
    ports = set()
    for ph in case.phases:
        for p in (ph.in_port, ph.exp_port):
            if p is not None and 0 <= p < 512:
                ports.add(p)
    for ent in case.snapshot_entities:
        if ent.table_name == "$pre.node":
            for d in ent.data:
                if d.field_name == "$DEV_PORT" and isinstance(d.value, list):
                    ports.update(int(x) for x in d.value)
    return ports


def runtime_rows(setup: List[snap.Entity], post: List[snap.Entity]) -> List[str]:
    """Table rows (not register cells) added, changed or removed between the two dumps."""
    old = {e.ident(): e for e in setup if not e.is_register}
    new = {e.ident(): e for e in post if not e.is_register}
    out = [f"+ {e}" for k, e in new.items() if k not in old]
    out += [f"~ {e}" for k, e in new.items() if k in old and old[k] != e]
    out += [f"- {e}" for k, e in old.items() if k not in new]
    return out


def drift(reference: List[snap.Entity], setup: List[snap.Entity]) -> List[str]:
    """The validation run's post-setup state against capture.py's, entry by entry."""
    ref = {ident(e): e for e in reference}
    got = {ident(e): e for e in setup}
    differ = [(ref[k], got[k]) for k in ref.keys() & got.keys()
              if (ref[k].action, ref[k].data) != (got[k].action, got[k].data)]
    only_ref = [ref[k] for k in ref.keys() - got.keys()]
    only_got = [got[k] for k in got.keys() - ref.keys()]
    out = [f"capture {len(ref)}  validate {len(got)}  differ {len(differ)}  "
           f"only-capture {len(only_ref)}  only-validate {len(only_got)}"]
    out += [f"DIFFER  {r.table} {r.keys}\n  capture:  {r.action} {r.data}\n"
            f"  validate: {g.action} {g.data}" for r, g in differ]
    out += [f"ONLY-CAPTURE   {e}" for e in only_ref]
    out += [f"ONLY-VALIDATE  {e}" for e in only_got]
    return out


class Validator:
    def __init__(self, program: str, cases: List[Tuple[str, Path, TamperCase]], work: Path,
                 settle: float, phase_timeout: float) -> None:
        self.program = program
        self.scen = capture.load_scenario(program)
        self.target = getattr(self.scen, "TARGET", "tofino")
        self.reactive = getattr(self.scen, "READY_MARKER", None)
        self.ctx = capture.prepare_work(program, self.scen, work, setup_only=False)
        self.ctx.ready_marker = self.reactive
        self.settle = settle
        self._setup: Optional[List[snap.Entity]] = None
        ports = {p.port for p in self.scen.PORTS}
        for _, _, case in cases:
            ports |= case_ports(case)
        self.ports = sorted(ports)
        if self.target == "bmv2":
            # simple_switch binds veth{2p}; packets go in and out through its peer veth{2p+1}.
            self.sw, self.p4info = capture.bmv2_switch(program, self.ctx, self.ports)
            iface = {p: f"veth{2 * p + 1}" for p in self.ports}
        else:
            # tofino-model owns veth{2p+1}; the host side is veth{2p} (ports.json veth1).
            self.ports_json = capture.write_ports_json(work / "ports.json", self.ports)
            iface = {p: f"veth{2 * p}" for p in self.ports}
        pcaps = work / "pcap"
        pcaps.mkdir(exist_ok=True)
        self.tester = PacketTester(client=None, phase_timeout=phase_timeout, capture_dir=pcaps,
                                   port_to_iface=iface)

    # --- one run on a fresh target ------------------------------------------
    def run(self, case: TamperCase, indices: List[int], tamper: List[int], label: str,
            want_setup: bool) -> Run:
        if self.target != "bmv2" and self.reactive:
            self._setup_state()  # a session of its own, before this run's
        with (self.ctx.work / "controller.log").open("a") as fd:
            fd.write(f"\n===== {label} =====\n")
        if self.target == "bmv2":
            return self._bmv2_run(case, indices, tamper, label, want_setup)
        return self._tofino_run(case, indices, tamper, label, want_setup)

    def _phases(self, case: TamperCase, indices: List[int], tamper: List[int], label: str,
                diag) -> Tuple[Dict[int, List[bytes]], str]:
        last: Dict[int, List[bytes]] = {}
        reg_diag = ""
        for i in indices:
            cap, _ = self.tester.replay(case, [i], label)
            if i not in tamper:
                last = cap
            time.sleep(self.settle)  # a reactive controller's response lands before the next phase
            if tamper and i == tamper[-1] and case.affected_registers:
                reg_diag = diag(case)
        return last, reg_diag

    def _tofino_run(self, case, indices, tamper, label, want_setup) -> Run:
        prog, ctx = self.program, self.ctx
        base = self._setup_state() if self.reactive else None
        if hasattr(self.scen, "switchd_argv"):
            switchd = capture.CustomSwitchd(self.scen.switchd_argv(ctx),
                                            ctx.work / "controller_switchd.log")
        else:
            switchd = td.BfSwitchdProcess(prog, capture.SDE, ctx.work / "bf_switchd.log")

        def body() -> Run:
            try:
                rc = capture._run_controller(self.scen, ctx)
                if self.reactive:
                    # The running controller owns the program binding, and BfRt serves no reads
                    # to an unbound client: send the phases, then stop it and read the state its
                    # rules left behind (they outlive its connection).
                    cap, diag = self._phases(case, indices, tamper, label,
                                             lambda c: "not read (the controller owns the program)")
                    ctx.stop_background()
                    client = self._reader()
                    try:
                        post = dump_bfrt.dump(client)[0]
                    finally:
                        client.close()
                    return Run(cap, diag, rc, base if want_setup else None,
                               runtime_rows(base, post))
                client = self._reader()
                try:
                    tester = PacketTester(client=client, phase_timeout=0,
                                          capture_dir=self.tester.capture_dir, port_to_iface={})
                    setup = dump_bfrt.dump(client)[0] if want_setup else None
                    cap, diag = self._phases(case, indices, tamper, label,
                                             lambda c: tester._diag_registers(c.affected_registers))
                finally:
                    client.close()
                return Run(cap, diag, rc, setup)
            finally:
                ctx.stop_background()
        return capture._tofino_session(prog, ctx, self.ports_json, "", switchd, body)

    def _reader(self, tries: int = 30) -> BfRtClient:
        """Client 1 (never the controller's 0), bound once the controller's binding is gone."""
        for n in range(tries):
            try:
                return BfRtClient(td.BFRT_GRPC_ADDR, client_id=1, p4_name=self.program,
                                  enable_learn=False)
            except Exception:  # noqa: BLE001 - the server drops a dead client's binding lazily
                if n == tries - 1:
                    raise
                time.sleep(1)
        raise AssertionError("unreachable")

    def _setup_state(self) -> List[snap.Entity]:
        """A reactive controller's post-setup state, from a session of its own with no packets
        (the runs can read the state only after their phases)."""
        if self._setup is None:
            prog, ctx = self.program, self.ctx
            switchd = td.BfSwitchdProcess(prog, capture.SDE, ctx.work / "bf_switchd.log")
            with (ctx.work / "controller.log").open("a") as fd:
                fd.write("\n===== setup state (no packets) =====\n")

            def body() -> List[snap.Entity]:
                try:
                    capture._run_controller(self.scen, ctx)
                finally:
                    ctx.stop_background()
                client = self._reader()
                try:
                    return dump_bfrt.dump(client)[0]
                finally:
                    client.close()
            self._setup = capture._tofino_session(prog, ctx, self.ports_json, "_setup", switchd,
                                                  body)
        return self._setup

    def _bmv2_registers(self, case: TamperCase) -> str:
        parts = []
        for reg in case.affected_registers:
            text = self.sw.cli(f"register_read {reg.register_name} {reg.index}\n")
            got = text.split("=")[-1].strip().split()[0] if "=" in text else "?"
            want = int.from_bytes(reg.attacker_value, "big")
            ok = got.isdigit() and int(got) == want
            parts.append(f"{reg.register_name}[{reg.index}]={want:x}:"
                         f"{'written' if ok else f'NOT-written (read {got})'}")
        return "; ".join(parts)

    def _bmv2_run(self, case, indices, tamper, label, want_setup) -> Run:
        from cp_snapshot import bmv2_live
        try:
            self.sw.start()
            rc = capture._run_controller(self.scen, self.ctx)
            dump = lambda: bmv2_live.to_entities(self.sw.dump(), self.p4info)[0]  # noqa: E731
            setup = dump() if want_setup or self.reactive else None
            cap, diag = self._phases(case, indices, tamper, label, self._bmv2_registers)
            rows = runtime_rows(setup, dump()) if self.reactive else []
            return Run(cap, diag, rc, setup, rows)
        finally:
            self.ctx.stop_background()
            self.sw.stop()


def load_baseline(path: Optional[Path]) -> Dict[str, str]:
    """case file name -> phase-0 replay status, from any CSV with "<case>.txtpb,<status>" rows."""
    out: Dict[str, str] = {}
    if path is None:
        return out
    with path.open() as fd:
        for row in csv.reader(fd):
            for i, cell in enumerate(row[:-1]):
                if cell.endswith(".txtpb"):
                    out[cell] = row[i + 1]
                    break
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program")
    ap.add_argument("cases", type=Path, help="directory of <category>/*.txtpb (or *.txtpb)")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--work", type=Path, help="default sp27/cp_live/<program>/_validate")
    ap.add_argument("--settle", type=float, default=1.0, help="seconds after each phase")
    ap.add_argument("--phase-timeout", type=float, default=2.0)
    ap.add_argument("--baseline", type=Path, help="phase-0 replay verdicts to compare with")
    ap.add_argument("--reference", type=Path,
                    help="capture.py's entities.pkl (default sp27/cp_live/<program>/entities.pkl)")
    ap.add_argument("--only", nargs="*", default=[], help="case-name substrings to keep")
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    cases_dir = a.cases.resolve()
    work = (a.work or capture.WORK / a.program / "_validate").resolve()
    out = (a.out or work / "out").resolve()
    reference = (a.reference or capture.WORK / a.program / "entities.pkl").resolve()
    baseline = load_baseline(a.baseline.resolve() if a.baseline else None)
    files = sorted(cases_dir.rglob("*.txtpb"))
    files = [f for f in files if not a.only or any(s in f.name for s in a.only)]
    cases = [(f.parent.name if f.parent != cases_dir else "", f, parse_tampering_txtpb(f))
             for f in files]
    if not cases:
        print(f"no cases under {cases_dir}", file=sys.stderr)
        return 2
    (out / "runtime").mkdir(parents=True, exist_ok=True)

    v = Validator(a.program, cases, work, a.settle, a.phase_timeout)
    created = capture.make_veths(v.ports)
    for p in v.ports:  # veths made earlier (veth_setup.sh, other tools) may still carry IPv6
        td.disable_ipv6(f"veth{2 * p}")
        td.disable_ipv6(f"veth{2 * p + 1}")
    rows = []
    try:
        for n, (cat, f, case) in enumerate(cases):
            legit_idx, attack_idx, tamper = run_indices(case)
            log.info("[%d/%d] %s/%s", n + 1, len(cases), cat, f.name)
            try:
                legit = v.run(case, legit_idx, [], f"{f.stem}_legit", want_setup=n == 0)
                attack = v.run(case, attack_idx, tamper, f"{f.stem}_attack", want_setup=False)
                ok, reason = v.tester.compare_runs(legit.capture, attack.capture,
                                                   case.diverge_output)
                status = "OK" if ok else "FAIL"
                if attack.reg_diag:
                    reason = f"{reason} [Phase-2 reg: {attack.reg_diag}]"
            except Exception as ex:  # noqa: BLE001 - one broken case does not stop the rest
                log.exception("%s", f.name)
                legit = attack = None
                status, reason = "ERROR", f"validate: {ex}"
            if n == 0 and legit is not None and legit.setup is not None:
                text = (drift(pickle.loads(reference.read_bytes())["after"], legit.setup)
                        if reference.exists() else [f"no reference ({reference})"])
                (out / "drift.txt").write_text(
                    f"{a.program}: post-setup state of run {f.stem}_legit vs {reference}\n"
                    + "\n".join(text) + "\n")
            rcs = "/".join(str(r.controller_rc) for r in (legit, attack) if r is not None)
            nrt = []
            for tag, r in (("legit", legit), ("attack", attack)):
                if r is not None and v.reactive:
                    (out / "runtime" / f"{f.stem}_{tag}.txt").write_text(
                        "".join(f"{line}\n" for line in r.runtime))
                    nrt.append(str(len(r.runtime)))
            base = baseline.get(f.name, "")
            agree = "" if not base else ("yes" if base == status else "no")
            rows.append([cat, f.name, status, reason, rcs, "/".join(nrt), base, agree])
            print(",".join(rows[-1][:3] + rows[-1][4:]), flush=True)
    finally:
        capture.drop_veths(created)
    with (out / "controller_replay.csv").open("w", newline="") as fd:
        w = csv.writer(fd)
        w.writerow(["category", "case", "status", "reason", "controller_rc", "runtime_rows",
                    "baseline", "agree"])
        w.writerows(rows)
    print(f"wrote {out / 'controller_replay.csv'}: "
          + ", ".join(f"{s} {sum(r[2] == s for r in rows)}" for s in ("OK", "FAIL", "ERROR")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
