"""Live bmv2 capture: simple_switch in the p4lang/behavioral-model container, dumped over Thrift.

Used by capture.py for scenarios with TARGET = "bmv2". The program is compiled with the local
p4c-bm2-ss (the same compiler whose P4Info names the static snapshots), loaded into simple_switch
on host veths (--net=host), configured by the scenario's controller step through
simple_switch_CLI, and dumped before and after it with bmv2_dump.py.
"""
from __future__ import annotations

import json
import logging
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from cp_snapshot.cli2snap import P4C_BM2_SS, P4INCLUDE, _by_name
from cp_snapshot.snapshot import Entity, Key

IMAGE = "p4lang/behavioral-model:latest"
CONTAINER = "cpsnap-bmv2"
THRIFT_PORT = 9191  # 9090 is tofino-model's
HERE = Path(__file__).resolve().parent

log = logging.getLogger("bmv2_live")


def compile_program(p4: Path, work: Path) -> Tuple[Path, dict]:
    out = work / "build"
    out.mkdir(parents=True, exist_ok=True)
    js, info = out / f"{p4.stem}.json", out / "p4info.json"
    subprocess.run([str(P4C_BM2_SS), "-I", str(P4INCLUDE), "--p4runtime-files", str(info),
                    "-o", str(js), str(p4)], check=True, capture_output=True)
    return js, json.loads(info.read_text())


class Switch:
    """simple_switch in a container sharing the host network (and so the host veths)."""

    def __init__(self, json_path: Path, ports: List[int], work: Path) -> None:
        self.json_path, self.ports, self.work = json_path, ports, work

    def start(self) -> None:
        subprocess.run(["docker", "rm", "-f", CONTAINER], capture_output=True)
        ifaces = [a for p in self.ports for a in ("-i", f"{p}@veth{2 * p}")]
        cmd = ["docker", "run", "-d", "--name", CONTAINER, "--net=host", "--privileged",
               "-v", f"{self.json_path.parent}:/prog:ro", "-v", f"{HERE}:/tools:ro", IMAGE,
               "simple_switch", "--log-console", "--thrift-port", str(THRIFT_PORT), *ifaces,
               f"/prog/{self.json_path.name}"]
        log.info("starting %s", " ".join(cmd))
        subprocess.run(cmd, check=True, capture_output=True)
        deadline = time.time() + 60
        while time.time() < deadline:
            r = self.exec(["simple_switch_CLI", "--thrift-port", str(THRIFT_PORT)], stdin="\n")
            if r.returncode == 0 and "Could not connect" not in r.stdout:
                return
            time.sleep(1)
        raise RuntimeError("simple_switch did not come up (docker logs cpsnap-bmv2)")

    def stop(self) -> None:
        logs = subprocess.run(["docker", "logs", CONTAINER], capture_output=True, text=True)
        (self.work / "simple_switch.log").write_text(logs.stdout + logs.stderr)
        subprocess.run(["docker", "rm", "-f", CONTAINER], capture_output=True)

    def exec(self, argv: List[str], stdin: Optional[str] = None) -> subprocess.CompletedProcess:
        return subprocess.run(["docker", "exec", "-i", CONTAINER, *argv], input=stdin,
                              capture_output=True, text=True)

    def cli(self, commands: str) -> str:
        r = self.exec(["simple_switch_CLI", "--thrift-port", str(THRIFT_PORT)], stdin=commands)
        return r.stdout + r.stderr

    def dump(self) -> dict:
        r = self.exec(["python3", "/tools/bmv2_dump.py", f"/prog/{self.json_path.name}",
                       str(THRIFT_PORT)])
        if r.returncode != 0:
            raise RuntimeError(f"bmv2_dump failed: {r.stderr[-2000:]}")
        return json.loads(r.stdout)


def _int(h: str) -> int:
    return int(h, 16) if h else 0


def to_entities(dump: dict, p4info: dict) -> Tuple[List[Entity], List[str]]:
    """Dump (bmv2 JSON names) -> snapshot entities named through P4Info, as cli2snap names them."""
    notes: List[str] = list(dump.get("errors", []))
    actions = {a["preamble"]["id"]: a for a in p4info.get("actions", [])}
    out: List[Entity] = []
    for tname, rec in dump["tables"].items():
        try:
            table = _by_name(p4info.get("tables", []), tname)
        except KeyError:
            continue  # compiler-generated (tbl_*) tables have no P4Info entry
        fields = table.get("matchFields", [])
        refs = [actions[r["id"]] for r in table.get("actionRefs", [])]

        def action(name: str, data: List[str]):
            a = _by_name(refs, name)
            params = a.get("params", [])
            return a["preamble"]["name"], tuple((p["name"], _int(d)) for p, d in zip(params, data))

        prio_table = any(f.get("matchType", "EXACT") in ("TERNARY", "RANGE", "OPTIONAL")
                         for f in fields)
        for e in rec["entries"]:
            keys = []
            for f, k in zip(fields, e["keys"]):
                if k["type"] == "ternary":
                    keys.append(Key(f["name"], "ternary", _int(k["key"]), _int(k["mask"])))
                elif k["type"] == "lpm":
                    keys.append(Key(f["name"], "lpm", _int(k["key"]), int(k["prefix"])))
                elif k["type"] == "range":
                    keys.append(Key(f["name"], "range", _int(k["key"]), _int(k["end"])))
                else:
                    keys.append(Key(f["name"], "exact", _int(k["key"])))
            name, params = action(e["action"], e["data"])
            # simple_switch: a smaller priority wins; P4Runtime: a larger one (as cli2snap).
            prio = (1 << 24) - e["priority"] if prio_table and e["priority"] >= 0 else 0
            out.append(Entity(table["preamble"]["name"], tuple(keys), name, params, prio))
        if rec["default"] is not None:
            try:
                name, params = action(rec["default"]["action"], rec["default"]["data"])
            except KeyError:
                continue  # NoAction and friends outside the table's P4Info action refs
            out.append(Entity(table["preamble"]["name"], (), name, params, 0, is_default=True))
    for rname, cells in dump["registers"].items():
        try:
            reg = _by_name(p4info.get("registers", []), rname)
        except KeyError:
            continue
        for i, v in enumerate(cells):
            out.append(Entity(f"register_id:{reg['preamble']['id']}",
                              (Key("$index", "exact", i),), "", (("value", int(v)),), 0,
                              is_register=True))
    mc = dump.get("mc") or {}
    l1 = {h["handle"]: h for h in mc.get("l1_handles", [])}
    l2 = {h["handle"]: h for h in mc.get("l2_handles", [])}
    for g in mc.get("mgrps", []):
        replicas = []
        for h in g.get("l1_handles", []):
            node = l1[h]
            for port in l2[node["l2_handle"]].get("ports", []):
                replicas.append((int(port), int(node["rid"])))
        out.append(Entity("$multicast_group", (Key("$MGID", "exact", int(g["id"])),), "",
                          (("replicas", sorted(replicas)),), 0))
    for sid, s in dump.get("mirror", {}).items():
        out.append(Entity("$clone_session", (Key("$SID", "exact", int(sid)),), "",
                          (("replicas", [(int(s["port"]), 0)]),), 0))
    return out, notes
