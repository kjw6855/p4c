"""Convert simple_switch_CLI command files into snapshot entities, resolved through P4Info.

Supported: table_add, table_set_default, register_write, mirroring_add, mc_mgrp_create /
mc_node_create / mc_node_associate. Names may be fully qualified or P4Info aliases. A line that
would fail on the switch (unknown table/action/register, wrong arity, unknown command such as the
`register_Write` typo) is skipped and reported, exactly as simple_switch_CLI would reject it.
Register writes of 0 are dropped: bmv2 registers start at 0, so they change nothing (a live
before/after capture drops them too).
"""

from __future__ import annotations

import ipaddress
import re
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List, Tuple

from cp_snapshot.snapshot import Entity, Key

P4C_BM2_SS = Path("/root/jiwon/Workspace/p4c/build/backends/bmv2/p4c-bm2-ss")
P4INCLUDE = Path("/root/jiwon/Workspace/p4c/p4include")


def load_p4info(p4_file: Path, extra_args: List[str] = ()) -> dict:
    """P4Info of `p4_file` (p4c-bm2-ss writes JSON when the file name ends in .json)."""
    import json
    with tempfile.TemporaryDirectory() as d:
        info = Path(d) / "p4info.json"
        subprocess.run([str(P4C_BM2_SS), "-I", str(P4INCLUDE), *extra_args, "--p4runtime-files",
                        str(info), "-o", str(Path(d) / "out.json"), str(p4_file)], check=True,
                       capture_output=True)
        return json.loads(info.read_text())


def _num(tok: str) -> int:
    tok = tok.strip()
    if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", tok):
        return int(ipaddress.ip_address(tok))
    if re.fullmatch(r"[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}", tok):
        return int(tok.replace(":", ""), 16)
    return int(tok, 0)


def _by_name(objs: List[dict], name: str) -> dict:
    hits = [o for o in objs if name in (o["preamble"]["name"], o["preamble"].get("alias"))
            or o["preamble"]["name"].endswith("." + name)]
    if len(hits) != 1:
        raise KeyError(f"{name!r} matches {len(hits)}")
    return hits[0]


class CliConverter:
    def __init__(self, p4info: dict) -> None:
        self.info = p4info
        self.actions = {a["preamble"]["id"]: a for a in p4info.get("actions", [])}
        self.entities: List[Entity] = []
        self.skipped: List[str] = []

    def _action(self, table: dict, name: str) -> dict:
        refs = [self.actions[r["id"]] for r in table.get("actionRefs", [])]
        return _by_name(refs, name)

    def _params(self, action: dict, toks: List[str]) -> Tuple[Tuple[str, int], ...]:
        params = action.get("params", [])
        if len(toks) != len(params):
            raise ValueError(f"{action['preamble']['name']} takes {len(params)} args, got {len(toks)}")
        return tuple((p["name"], _num(t)) for p, t in zip(params, toks))

    def _match(self, table: dict, toks: List[str]) -> Tuple[Tuple[Key, ...], List[str]]:
        fields = table.get("matchFields", [])
        if len(toks) < len(fields):
            raise ValueError(f"{table['preamble']['name']} needs {len(fields)} match fields")
        keys = []
        for f, tok in zip(fields, toks):
            kind = f.get("matchType", "EXACT").lower()
            if kind == "ternary":
                v, m = tok.split("&&&")
                keys.append(Key(f["name"], "ternary", _num(v), _num(m)))
            elif kind == "lpm":
                v, n = tok.split("/")
                keys.append(Key(f["name"], "lpm", _num(v), int(n)))
            elif kind == "range":
                lo, hi = tok.split("->")
                keys.append(Key(f["name"], "range", _num(lo), _num(hi)))
            elif kind == "optional":
                if tok != "*":
                    keys.append(Key(f["name"], "optional", _num(tok)))
            else:
                keys.append(Key(f["name"], "exact", _num(tok)))
        return tuple(keys), toks[len(fields):]

    def line(self, raw: str) -> None:
        text = raw.split("#", 1)[0].strip()
        if not text:
            return
        toks = shlex.split(text)
        cmd, args = toks[0], toks[1:]
        try:
            if cmd == "table_add":
                table = _by_name(self.info["tables"], args[0])
                action = self._action(table, args[1])
                arrow = args.index("=>")
                keys, rest = self._match(table, args[2:arrow])
                if rest:
                    raise ValueError(f"extra match tokens {rest}")
                after = args[arrow + 1:]
                n = len(action.get("params", []))
                params = self._params(action, after[:n])
                prio = 0
                if len(after) > n:
                    # simple_switch_CLI: a smaller priority wins; P4Runtime: a larger one does.
                    prio = (1 << 24) - int(after[n])
                self.entities.append(Entity(table["preamble"]["name"], keys,
                                            action["preamble"]["name"], params, prio))
            elif cmd == "table_set_default":
                table = _by_name(self.info["tables"], args[0])
                action = self._action(table, args[1])
                self.entities.append(Entity(table["preamble"]["name"], (),
                                            action["preamble"]["name"],
                                            self._params(action, args[2:]), 0, is_default=True))
            elif cmd == "register_write":
                reg = _by_name(self.info.get("registers", []), args[0])
                value = _num(args[2])
                if value != 0:
                    self.entities.append(Entity(f"register_id:{reg['preamble']['id']}",
                                                (Key("$index", "exact", int(args[1])),), "",
                                                (("value", value),), 0, is_register=True))
            elif cmd == "mirroring_add":
                self.entities.append(Entity("$clone_session", (Key("$SID", "exact", int(args[0])),),
                                            "", (("replicas", [(int(args[1]), 0)]),), 0))
            else:
                raise ValueError(f"unknown command {cmd!r}")
        except (KeyError, ValueError, IndexError) as e:
            self.skipped.append(f"{raw.strip()}  -- {e}")


def convert(p4_file: Path, cli_files: List[Path], extra_args: List[str] = ()) -> CliConverter:
    conv = CliConverter(load_p4info(p4_file, extra_args))
    for f in cli_files:
        for raw in Path(f).read_text().splitlines():
            conv.line(raw)
    return conv
