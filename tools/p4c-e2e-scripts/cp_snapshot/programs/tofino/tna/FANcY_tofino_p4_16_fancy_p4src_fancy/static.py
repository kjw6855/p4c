"""FANcY: tofino/p4_16/fancy/control_plane/control_plane.py FancyController.configure_all().

configure_all computes its rules from includes/constants.p4 (state-machine values, port ids), so the
script's own methods are evaluated against a recording stand-in for its BfRtAPI helper instead of
being retyped; the recorded calls go through the bf-rt.json-checked builder. Model mode ("model"
argument), i.e. ports PORTn_M = 0..6: the script's argparse rejects the hardware-mode positional, and
the model ports exist on tofino-model.
"""
import ipaddress
import os
import sys
import types
from pathlib import Path

from cp_snapshot.snapshot import Port

TARGET = "tofino"
CP_DIR = Path("/root/jiwon/Workspace-remote/top_tier_repo/FANcY/tofino/p4_16/fancy/control_plane")
SOURCES = ["FANcY/tofino/p4_16/fancy/control_plane/control_plane.py:configure_all (model mode)",
           "FANcY/tofino/p4_16/fancy/includes/constants.p4"]
NOTES = ["model ports PORT0_M..PORT6_M = 0..6; attacker on 7"]
PORTS = [Port(p, "HOST", f"p{p}") for p in range(7)] + [Port(7, "ATTACKER", "attacker")]


class _Recorder:
    def __init__(self, *_a, **_k):
        self.calls = []
        # The script reads table.info.key_dict only to overwrite it with a static key list.
        table = types.SimpleNamespace(info=types.SimpleNamespace(key_dict={}))
        self.bfrt_info = types.SimpleNamespace(table_get=lambda _name: table)
        _Recorder.last = self

    def clear_all(self):
        pass

    def entry_add(self, table_name, keys=(), data=(), action_name=None):
        self.calls.append(("entry", table_name, list(keys), list(data), action_name))

    def default_entry_set(self, table_name, data=(), action_name=None):
        self.calls.append(("default", table_name, [], list(data), action_name))

    def add_mirroring(self, eg_port, session_id, direction="BOTH"):
        self.calls.append(("mirror", eg_port, session_id, direction))

    def register_entry_add(self, reg_name, index, value):
        self.calls.append(("register", reg_name, index, value))


def _int(v):
    if isinstance(v, str):
        return int(ipaddress.ip_address(v)) if "." in v else int(v, 0)
    return int(v)


def _record():
    stubs = {name: types.ModuleType(name) for name in ("ipdb", "send_traffic", "server")}
    stubs["server"].TofinoCommandServer = object
    helper = types.ModuleType("bfrt_grpc_helper")
    helper.BfRtAPI = _Recorder
    helper.gc = types.SimpleNamespace()
    stubs["bfrt_grpc_helper"] = helper
    saved = (sys.argv, os.getcwd(), list(sys.path))
    sys.modules.update(stubs)
    try:
        os.chdir(CP_DIR)
        sys.argv = ["control_plane.py", "model"]
        sys.path.insert(0, str(CP_DIR))
        ns = {"__name__": "fancy_control_plane", "__file__": str(CP_DIR / "control_plane.py")}
        exec(compile((CP_DIR / "control_plane.py").read_text(), "control_plane.py", "exec"), ns)
        ns["FancyController"]().configure_all()
    finally:
        sys.argv, cwd, sys.path[:] = saved[0], saved[1], saved[2]
        os.chdir(cwd)
        for name in stubs:
            sys.modules.pop(name, None)
    return _Recorder.last.calls


def build(r):
    for call in _record():
        kind = call[0]
        if kind == "mirror":
            _, port, sid, direction = call
            r.mirror_session(sid, port, direction)
        elif kind == "register":
            _, reg, idx, value = call
            r.register(reg, idx, _int(value))
        else:
            _, table, keys, data, action = call
            t = r._table(table)
            kd, prio = {}, 0
            for k in keys:
                name, rest = k[0], k[1:]
                if name == "$MATCH_PRIORITY":
                    prio = _int(rest[0])
                    continue
                kind_ = r._one(t["key"], name, "key", t["name"])["match_type"].lower()
                if kind_ == "ternary":
                    kd[name] = ("ternary", _int(rest[0]), _int(rest[1]))
                elif kind_ == "lpm":
                    kd[name] = ("lpm", _int(rest[0]), _int(rest[2] if len(rest) > 2 else rest[1]))
                elif kind_ == "range":
                    kd[name] = ("range", _int(rest[0]), _int(rest[1]))
                else:
                    kd[name] = _int(rest[0])
            params = {d[0]: _int(d[1]) for d in data}
            if kind == "default":
                r.default(table, action, params)
            else:
                r.entry(table, action, kd, params, priority=prio)
