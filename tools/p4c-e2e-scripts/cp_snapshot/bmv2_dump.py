"""Dump a running bmv2 simple_switch's control-plane state over Thrift, as JSON on stdout.

Runs inside the p4lang/behavioral-model container (its venv has bm_runtime / sswitch_runtime):

    python3 bmv2_dump.py <bmv2.json> [thrift_port]

Reads, for every table of the JSON: its entries and default entry; every register array cell; the
multicast groups; and the mirroring sessions. Names stay bmv2 JSON names; bmv2_live.py resolves
them through P4Info on the host.
"""
import json
import sys

import runtime_CLI
from bm_runtime.standard.ttypes import BmMatchParamType
from sswitch_runtime import SimpleSwitch

MAX_MIRROR_SESSIONS = 1024


def _hex(b):
    return b.hex() if isinstance(b, (bytes, bytearray)) else bytes(b, "latin-1").hex()


def _key(p):
    t = p.type
    if t == BmMatchParamType.EXACT:
        return {"type": "exact", "key": _hex(p.exact.key)}
    if t == BmMatchParamType.LPM:
        return {"type": "lpm", "key": _hex(p.lpm.key), "prefix": p.lpm.prefix_length}
    if t == BmMatchParamType.TERNARY:
        return {"type": "ternary", "key": _hex(p.ternary.key), "mask": _hex(p.ternary.mask)}
    if t == BmMatchParamType.RANGE:
        return {"type": "range", "key": _hex(p.range.start), "end": _hex(p.range.end_)}
    return {"type": "valid", "key": "01" if p.valid.key else "00"}


def _action(a):
    return {"action": a.action_name, "data": [_hex(d) for d in (a.action_data or [])]}


def main():
    cfg = json.load(open(sys.argv[1]))
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9090
    services = runtime_CLI.RuntimeAPI.get_thrift_services(runtime_CLI.PreType.SimplePreLAG)
    std, mc = runtime_CLI.thrift_connect("localhost", port, services)
    sswitch, = runtime_CLI.thrift_connect("localhost", port,
                                          [("simple_switch", SimpleSwitch.Client)])
    out = {"tables": {}, "registers": {}, "mc": None, "mirror": {}, "errors": []}
    for pipe in cfg.get("pipelines", []):
        for t in pipe.get("tables", []):
            name = t["name"]
            rec = {"entries": [], "default": None}
            try:
                for e in std.bm_mt_get_entries(0, name):
                    rec["entries"].append({"keys": [_key(k) for k in e.match_key],
                                           "priority": e.options.priority if e.options else -1,
                                           **_action(e.action_entry)})
                rec["default"] = _action(std.bm_mt_get_default_entry(0, name))
            except Exception as ex:  # noqa: BLE001 - e.g. indirect tables
                out["errors"].append(f"table {name}: {ex}")
            out["tables"][name] = rec
    for r in cfg.get("register_arrays", []):
        try:
            out["registers"][r["name"]] = list(std.bm_register_read_all(0, r["name"]))
        except Exception as ex:  # noqa: BLE001
            out["errors"].append(f"register {r['name']}: {ex}")
    try:
        out["mc"] = json.loads(mc.bm_mc_get_entries(0))
    except Exception as ex:  # noqa: BLE001
        out["errors"].append(f"mc: {ex}")
    for sid in range(MAX_MIRROR_SESSIONS):
        try:
            s = sswitch.mirroring_session_get(sid)
        except Exception:  # noqa: BLE001 - no such session
            continue
        out["mirror"][sid] = {"port": s.port, "mgid": s.mgid}
    json.dump(out, sys.stdout)


if __name__ == "__main__":
    main()
