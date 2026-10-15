"""Build tofino snapshot entities from a program's static rules, checked against its bf-rt.json.

Generators in static/ transcribe what a program's setup script installs (bfrt_python, BfRt gRPC,
PTF or C BfRt calls with literal or loop-computed values). Every table, key, action, parameter and
register name is resolved against bf-rt.json by dotted suffix, so a typo or a name the program does
not have fails here rather than in p4symbex.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Union

from cp_snapshot.snapshot import Entity, Key

TOFINOPD = Path("/root/jiwon/Workspace/open-p4studio/install/share/tofinopd")
KeyValue = Union[int, Tuple]  # int | ("ternary", v, mask) | ("lpm", v, len) | ("range", lo, hi)


def _suffix_match(full: str, short: str) -> bool:
    return full == short or full.endswith("." + short)


class BfrtRules:
    def __init__(self, program: str, bfrt_json: Optional[Path] = None,
                 schema_optional: bool = False) -> None:
        path = Path(bfrt_json or TOFINOPD / program / "bf-rt.json")
        if not path.exists() and schema_optional:
            # Only fixed tables ($pre, $mirror) can be written without a schema.
            self.schema = {"tables": []}
        else:
            self.schema = json.loads(path.read_text())
        self.tables = {t["name"]: t for t in self.schema["tables"]}
        self.entities: List[Entity] = []

    # --- resolution -------------------------------------------------------------------------
    def _table(self, short: str, types=("MatchAction_Direct", "MatchAction_Indirect")) -> dict:
        hits = [t for n, t in self.tables.items()
                if _suffix_match(n, short) and t["table_type"] in types]
        if len(hits) != 1:
            raise KeyError(f"table {short!r}: {len(hits)} matches among {types}")
        return hits[0]

    @staticmethod
    def _one(items: List[dict], short: str, what: str, where: str) -> dict:
        hits = [i for i in items if _suffix_match(i["name"], short)]
        if len(hits) != 1:
            names = [i["name"] for i in items]
            raise KeyError(f"{where}: {what} {short!r} matches {len(hits)} of {names}")
        return hits[0]

    def _action(self, table: dict, short: str) -> Tuple[str, List[dict]]:
        spec = self._one(table.get("action_specs", []), short, "action", table["name"])
        return spec["name"], spec.get("data", [])

    @staticmethod
    def _width(field: dict) -> int:
        return int(field["type"].get("width", 1))

    # --- entries ----------------------------------------------------------------------------
    def _keys(self, table: dict, keys: Dict[str, KeyValue]) -> Tuple[Key, ...]:
        out = []
        for short, v in keys.items():
            field = self._one(table["key"], short, "key", table["name"])
            name, kind, width = field["name"], field["match_type"].lower(), self._width(field)
            allbits = (1 << width) - 1
            if isinstance(v, tuple):
                kind_given, a, b = v
                if kind_given != kind:
                    raise ValueError(f"{table['name']}.{name}: {kind_given} given for {kind} key")
                out.append(Key(name, kind, int(a), int(b) & allbits if kind == "ternary" else int(b)))
            elif kind == "ternary":
                out.append(Key(name, kind, int(v), allbits))
            elif kind == "lpm":
                out.append(Key(name, kind, int(v), width))
            elif kind == "range":
                out.append(Key(name, kind, int(v), int(v)))
            else:
                out.append(Key(name, kind, int(v)))
        given = {k.name for k in out}
        missing = [f["name"] for f in table["key"]
                   if f["match_type"] == "Exact" and f["name"] not in given
                   and f["name"] != "$MATCH_PRIORITY"]
        if missing:
            raise ValueError(f"{table['name']}: exact keys without a value: {missing}")
        return tuple(out)

    def _params(self, table: dict, action: str, fields: List[dict],
                params: Dict[str, int]) -> Tuple[Tuple[str, int], ...]:
        out = []
        for short, v in params.items():
            f = self._one(fields, short, "parameter", f"{table['name']} {action}")
            out.append((f["name"], int(v)))
        missing = [f["name"] for f in fields if f["name"] not in {n for n, _ in out}]
        if missing:
            raise ValueError(f"{table['name']} {action}: parameters without a value: {missing}")
        return tuple(sorted(out))

    def entry(self, table: str, action: str, keys: Dict[str, KeyValue],
              params: Optional[Dict[str, int]] = None, priority: int = 0) -> None:
        t = self._table(table)
        aname, fields = self._action(t, action)
        self.entities.append(Entity(t["name"], self._keys(t, keys), aname,
                                    self._params(t, aname, fields, params or {}), priority))

    def default(self, table: str, action: str, params: Optional[Dict[str, int]] = None) -> None:
        t = self._table(table)
        aname, fields = self._action(t, action)
        self.entities.append(Entity(t["name"], (), aname,
                                    self._params(t, aname, fields, params or {}), 0,
                                    is_default=True))

    def register(self, register: str, index: int, value: Union[int, Dict[str, int]]) -> None:
        t = self._table(register, types=("Register",))
        fields = [d["singleton"] for d in t.get("data", []) if "singleton" in d
                  and not d["singleton"]["name"].startswith("$")]
        if isinstance(value, int):
            if len(fields) != 1:
                raise ValueError(f"{t['name']} has fields {[f['name'] for f in fields]}")
            data = ((fields[0]["name"], int(value)),)
        else:
            data = tuple(sorted((self._one(fields, k, "field", t["name"])["name"], int(v))
                                for k, v in value.items()))
        self.entities.append(Entity(t["name"], (Key("$REGISTER_INDEX", "exact", int(index)),),
                                    "", data, 0, is_register=True))

    def register_param(self, param: str, value: int) -> None:
        t = self._table(param, types=("RegisterParam",))
        self.entities.append(Entity(t["name"], (), "", (("value", int(value)),), 0))

    # --- fixed tables (replay-only for p4symbex; multicast also feeds the output oracle) -----
    def multicast_group(self, mgid: int, ports: List[int], rid: int = 0) -> None:
        """Group `mgid` replicating to `ports`, all with replication id `rid`."""
        self.multicast_replicas(mgid, [(p, rid) for p in ports])

    def multicast_replicas(self, mgid: int, replicas: List[Tuple[int, int]]) -> None:
        """Group `mgid` with (port, rid) replicas: one L1 node per distinct rid. Node ids are
        mgid * 16 + k, so groups never share a node."""
        rids = sorted({rid for _, rid in replicas})
        nodes = []
        for k, rid in enumerate(rids):
            node_id = mgid * 16 + k
            ports = sorted(p for p, r in replicas if r == rid)
            self.entities.append(Entity("$pre.node",
                                        (Key("$MULTICAST_NODE_ID", "exact", node_id),), "",
                                        (("$DEV_PORT", ports), ("$MULTICAST_RID", int(rid))), 0))
            nodes.append(node_id)
        self.entities.append(Entity("$pre.mgid", (Key("$MGID", "exact", mgid),), "",
                                    (("$MULTICAST_NODE_ID", nodes),
                                     ("$MULTICAST_NODE_L1_XID", [0] * len(nodes)),
                                     ("$MULTICAST_NODE_L1_XID_VALID", [False] * len(nodes))), 0))

    def mirror_session(self, sid: int, port: int, direction: str = "BOTH",
                       max_pkt_len: int = 16384) -> None:
        self.entities.append(Entity("$mirror.cfg", (Key("$sid", "exact", sid),), "normal",
                                    (("$direction", direction), ("$max_pkt_len", max_pkt_len),
                                     ("$session_enable", True), ("$ucast_egress_port", port),
                                     ("$ucast_egress_port_valid", True)), 0))
