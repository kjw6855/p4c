"""Evaluate a bfrt_python setup script's static rules without a switch.

The script runs with `bfrt` bound to a recorder that turns `<table>.add_with_<action>(**kw)`,
`set_default_with_<action>`, register `mod/add(REGISTER_INDEX=..)` and the fixed `mirror.cfg` /
`pre.node` / `pre.mgid` tables into BfrtRules calls, which resolve every name against bf-rt.json.
Port, traffic-manager and pktgen calls configure the device, not the pipeline, and are ignored.
Imports of socket/subprocess/os/time are answered by stand-ins (hostname, no child processes, no
sleeps), and `path_map` rewrites host paths the script opens to their location in the repo.
"""

from __future__ import annotations

import builtins
import os
import re
import types
from pathlib import Path
from typing import Dict, Optional

from cp_snapshot.bfrt_rules import BfrtRules
from cp_snapshot.snapshot import Entity, Key

IGNORED_ROOTS = ("port", "tf1", "tf2", "pktgen", "tm")


def _fold(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z]", "_", name)


class _Node:
    """One attribute path below `bfrt`, e.g. receiver.pipe.SwitchIngress.l2_forward."""

    def __init__(self, rec: "_Recorder", path: tuple) -> None:
        self._rec, self._path = rec, path

    def __getattr__(self, name: str):
        if name.startswith("add_with_"):
            return lambda **kw: self._rec.entry(self._path, name[len("add_with_"):], kw)
        if name.startswith("set_default_with_"):
            return lambda **kw: self._rec.default(self._path, name[len("set_default_with_"):], kw)
        if name.startswith("mod_with_"):
            return lambda **kw: self._rec.ignored(self._path, name, kw)
        if name in ("add", "mod"):
            return lambda **kw: self._rec.plain(self._path, name, kw)
        if name in ("clear", "delete", "dump", "info", "reset"):
            return lambda *a, **kw: None
        if name == "get":
            # Port lookups need a device port; pipeline tables start empty, so a read-back finds
            # no entry (bfrt_python returns -1).
            if self._path and self._path[0] in IGNORED_ROOTS:
                return lambda *a, **kw: types.SimpleNamespace(data={b"$DEV_PORT": 0})
            return lambda *a, **kw: -1
        return _Node(self._rec, self._path + (name,))

    def __call__(self, *a, **kw):
        return None


class _Recorder:
    def __init__(self, rules: BfrtRules, program_name: str) -> None:
        self.rules, self.program_name = rules, program_name
        self.ignored_calls = []

    # bfrt.<program>.<pipe>.<control>.<table> -> "<control>.<table>" for suffix resolution
    def _table_name(self, path: tuple) -> str:
        if path and path[0] == self.program_name:
            path = path[2:]  # drop program and pipeline names
        return ".".join(path)

    def _keys_and_params(self, table: dict, kw: Dict) -> tuple:
        kw = dict(kw)
        prio = int(kw.pop("MATCH_PRIORITY", 0))
        keys = {}
        for field in table["key"]:
            name = field["name"]
            if name == "$MATCH_PRIORITY":
                continue
            short = _fold(name.split(".")[-1]) if "." in name else _fold(name)
            cand = [c for c in (_fold(name), short) if c in kw or f"{c}_start" in kw]
            if not cand:
                continue
            c = cand[0]
            kind = field["match_type"].lower()
            if kind == "ternary":
                keys[name] = ("ternary", kw.pop(c), kw.pop(f"{c}_mask", (1 << 128) - 1))
            elif kind == "lpm":
                keys[name] = ("lpm", kw.pop(c), kw.pop(f"{c}_p_length", kw.pop(f"{c}_prefix_len", 128)))
            elif kind == "range":
                keys[name] = ("range", kw.pop(f"{c}_start"), kw.pop(f"{c}_end"))
            else:
                keys[name] = kw.pop(c)
        return keys, kw, prio

    def entry(self, path: tuple, action: str, kw: Dict) -> None:
        if path[0] in IGNORED_ROOTS:
            return self.ignored(path, "add_with_" + action, kw)
        if path[:2] == ("mirror", "cfg"):
            return self.fixed("$mirror.cfg", ("$sid", kw.pop("sid")), action, kw)
        t = self.rules._table(self._table_name(path))
        keys, params, prio = self._keys_and_params(t, kw)
        self.rules.entry(t["name"], action, keys, params, priority=prio)

    def default(self, path: tuple, action: str, kw: Dict) -> None:
        self.rules.default(self._table_name(path), action, kw)

    def plain(self, path: tuple, verb: str, kw: Dict) -> None:
        if path[0] in IGNORED_ROOTS:
            return self.ignored(path, verb, kw)
        if path[:2] == ("pre", "node"):
            return self.fixed("$pre.node", ("$MULTICAST_NODE_ID", kw.pop("MULTICAST_NODE_ID")), "", kw)
        if path[:2] == ("pre", "mgid"):
            return self.fixed("$pre.mgid", ("$MGID", kw.pop("MGID")), "", kw)
        name = self._table_name(path)
        if path[-1] == "PORT_METADATA":
            # Replay-only parser state ($PORT_METADATA); p4symbex ignores it.
            key = next(k for k in kw if "port" in k.lower())
            pipe_path = path[1:-1] if path[0] == self.program_name else path[:-1]
            return self.fixed(".".join(pipe_path + ("$PORT_METADATA",)),
                              ("ig_intr_md.ingress_port", kw.pop(key)), "", kw)
        if "REGISTER_INDEX" in kw:
            idx = kw.pop("REGISTER_INDEX")
            t = self.rules._table(name, types=("Register",))
            fields = {k.split(".")[-1]: v for k, v in kw.items()}
            value = fields if len(fields) > 1 else next(iter(fields.values()))
            return self.rules.register(t["name"], idx, value)
        self.ignored(path, verb, kw)

    def fixed(self, table: str, key: tuple, action: str, kw: Dict) -> None:
        data = tuple(sorted((k if k.startswith("$") else "$" + k,
                             v if isinstance(v, (list, str)) else int(v)) for k, v in kw.items()))
        self.rules.entities.append(Entity(table, (Key(key[0], "exact", int(key[1])),), action,
                                          data, 0))

    def ignored(self, path: tuple, verb: str, kw: Dict) -> None:
        self.ignored_calls.append(f"{'.'.join(path)}.{verb}")


def run_bfrt_python(rules: BfrtRules, script: Path, program_name: str, hostname: str = "",
                    path_map: Optional[Dict[str, str]] = None) -> _Recorder:
    rec = _Recorder(rules, program_name)
    path_map = path_map or {}
    real_open, real_import = builtins.open, builtins.__import__

    def mapped(p):
        p = str(p)
        for host, local in path_map.items():
            if p.startswith(host):
                return local + p[len(host):]
        return p

    fake_socket = types.SimpleNamespace(gethostname=lambda: hostname)
    fake_subprocess = types.SimpleNamespace(check_output=lambda *a, **k: b"",
                                            run=lambda *a, **k: None, call=lambda *a, **k: 0,
                                            Popen=lambda *a, **k: None)
    fake_os = types.SimpleNamespace(**{n: getattr(os, n) for n in dir(os) if not n.startswith("_")})
    fake_os.system = lambda *_a: 0
    fake_time = types.SimpleNamespace(sleep=lambda *_a: None, time=lambda: 0.0)
    fakes = {"socket": fake_socket, "subprocess": fake_subprocess, "os": fake_os,
             "time": fake_time, "bfrtcli": types.SimpleNamespace(bfrt=_Node(rec, ()))}

    class _Inert(types.ModuleType):
        """A module the host lacks (helper libraries of devtest scripts): every attribute is a
        no-op callable, so definitions that use it load and are never run."""

        def __getattr__(self, _name):
            return lambda *a, **k: None

    def fake_import(name, *a, **k):
        if name in fakes:
            return fakes[name]
        try:
            return real_import(name, *a, **k)
        except ImportError:
            rec.ignored_calls.append(f"import {name} (unavailable; inert)")
            return _Inert(name)

    sandbox_builtins = dict(vars(builtins), open=lambda p, *a, **k: real_open(mapped(p), *a, **k),
                            __import__=fake_import)
    ns = {"__name__": "__main__", "__file__": str(script), "__builtins__": sandbox_builtins,
          "bfrt": _Node(rec, ())}
    exec(compile(Path(script).read_text(), str(script), "exec"), ns)
    return rec
