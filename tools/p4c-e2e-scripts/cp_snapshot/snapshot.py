"""Target-neutral model of captured control-plane state and its symbex.CpSnapshot textproto writer.

A capture reads the target twice -- after the pipeline is loaded and after the controller ran -- and
keeps only what changed (`diff`). That drops what the program itself fixes (const entries, compiled
default actions, declared register initial values), so the snapshot holds exactly the state the
controller installed.
"""

from __future__ import annotations

import datetime
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple, Union

Value = Union[int, bool, str, List[int], List[bool]]


@dataclass(frozen=True)
class Key:
    name: str
    kind: str  # exact | ternary | lpm | range | optional
    value: int
    second: Optional[int] = None  # ternary mask, lpm prefix_len, range high


@dataclass
class Entity:
    """One table entry, default entry, register cell or fixed-table ($pre/$mirror) entry."""

    table: str
    keys: Tuple[Key, ...] = ()
    action: str = ""
    data: Tuple[Tuple[str, Value], ...] = ()
    priority: int = 0
    is_default: bool = False
    is_register: bool = False

    def ident(self) -> tuple:
        """What makes two captures of the same entry the same entry (the key, not the data)."""
        return (self.table, self.is_default, self.keys, self.priority if self.keys else 0)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Entity) and (self.ident(), self.action, self.data) == (
            other.ident(), other.action, other.data)

    def __hash__(self) -> int:
        return hash(self.ident())


def diff(before: List[Entity], after: List[Entity]) -> List[Entity]:
    """Entities of `after` that are new or changed relative to `before`."""
    old = {e.ident(): e for e in before}
    return [e for e in after if old.get(e.ident()) != e]


@dataclass
class Port:
    port: int
    role: str  # HOST | LINK | ATTACKER | CPU | RECIRC
    name: str = ""
    mac: str = ""
    ip: str = ""


@dataclass
class Snapshot:
    program: str
    target: str  # bmv2 | tofino
    ports: List[Port] = field(default_factory=list)
    entities: List[Entity] = field(default_factory=list)
    snapshot_id: str = "post-setup"
    captured_at: str = field(
        default_factory=lambda: datetime.datetime.now(datetime.timezone.utc).isoformat())
    notes: List[str] = field(default_factory=list)


def _bytes(v: int) -> str:
    """Minimal big-endian byte string of non-negative int `v`, as a textproto string literal."""
    v = int(v)
    if v < 0:
        raise ValueError(f"negative value {v}")
    n = max(1, (v.bit_length() + 7) // 8)
    return '"' + "".join(f"\\x{b:02x}" for b in v.to_bytes(n, "big")) + '"'


def _quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _bfrt_entity(e: Entity, ind: str) -> List[str]:
    out = [f"{ind}table_name: {_quote(e.table)}"]
    if e.action:
        out.append(f"{ind}action_name: {_quote(e.action)}")
    for k in e.keys:
        if k.kind == "exact":
            m = f"exact {{ value: {_bytes(k.value)} }}"
        elif k.kind == "ternary":
            m = f"ternary {{ value: {_bytes(k.value)} mask: {_bytes(k.second)} }}"
        elif k.kind == "lpm":
            m = f"lpm {{ value: {_bytes(k.value)} prefix_len: {int(k.second)} }}"
        elif k.kind == "range":
            m = f"range {{ low: {_bytes(k.value)} high: {_bytes(k.second)} }}"
        else:
            m = f"optional {{ value: {_bytes(k.value)} }}"
        out.append(f"{ind}key {{ field_name: {_quote(k.name)} {m} }}")
    for name, v in e.data:
        if isinstance(v, bool):
            body = f"value: {_bytes(int(v))}"
        elif isinstance(v, int):
            body = f"value: {_bytes(v)}"
        elif isinstance(v, str):
            body = f"str_val: {_quote(v)}"
        elif v and all(isinstance(x, bool) for x in v):
            body = " ".join(f"bool_arr_val: {'true' if x else 'false'}" for x in v)
        else:
            body = " ".join(f"int_arr_val: {int(x)}" for x in v)
        out.append(f"{ind}data {{ field_name: {_quote(name)} {body} }}")
    if e.priority and not e.is_register:
        out.append(f"{ind}priority: {int(e.priority)}")
    if e.is_default:
        out.append(f"{ind}is_default_entry: true")
    return out


def _p4rt_entity(e: Entity, ind: str) -> List[str]:
    """A P4Runtime Entity (names via the fork's name fields); registers carry their P4Info id in
    `table` as "register_id:<id>"."""
    i2 = ind + "  "
    if e.is_register:
        rid = int(e.table.split(":", 1)[1])
        idx = next(k.value for k in e.keys if k.name == "$index")
        (_, val), = e.data
        return [f"{ind}register_entry {{",
                f"{i2}register_id: {rid}",
                f"{i2}index {{ index: {int(idx)} }}",
                f"{i2}data {{ bitstring: {_bytes(int(val))} }}",
                f"{ind}}}"]
    if e.table == "$multicast_group":
        mgid = next(k.value for k in e.keys if k.name == "$MGID")
        out = [f"{ind}packet_replication_engine_entry {{", f"{i2}multicast_group_entry {{",
               f"{i2}  multicast_group_id: {int(mgid)}"]
        for port, rid in dict(e.data)["replicas"]:
            out.append(f"{i2}  replicas {{ port: {_bytes(port)} instance: {int(rid)} }}")
        return out + [f"{i2}}}", f"{ind}}}"]
    if e.table == "$clone_session":
        sid = next(k.value for k in e.keys if k.name == "$SID")
        out = [f"{ind}packet_replication_engine_entry {{", f"{i2}clone_session_entry {{",
               f"{i2}  session_id: {int(sid)}"]
        for port, rid in dict(e.data)["replicas"]:
            out.append(f"{i2}  replicas {{ port: {_bytes(port)} instance: {int(rid)} }}")
        return out + [f"{i2}}}", f"{ind}}}"]
    out = [f"{ind}table_entry {{", f"{i2}table_name: {_quote(e.table)}"]
    for k in e.keys:
        if k.kind == "exact":
            m = f"exact {{ value: {_bytes(k.value)} }}"
        elif k.kind == "ternary":
            m = f"ternary {{ value: {_bytes(k.value)} mask: {_bytes(k.second)} }}"
        elif k.kind == "lpm":
            m = f"lpm {{ value: {_bytes(k.value)} prefix_len: {int(k.second)} }}"
        elif k.kind == "range":
            m = f"range {{ low: {_bytes(k.value)} high: {_bytes(k.second)} }}"
        else:
            m = f"optional {{ value: {_bytes(k.value)} }}"
        out.append(f"{i2}match {{ field_name: {_quote(k.name)} {m} }}")
    params = " ".join(f"params {{ param_name: {_quote(n)} value: {_bytes(int(v))} }}"
                      for n, v in e.data)
    out.append(f"{i2}action {{ action {{ action_name: {_quote(e.action)} {params} }} }}")
    if e.priority:
        out.append(f"{i2}priority: {int(e.priority)}")
    if e.is_default:
        out.append(f"{i2}is_default_action: true")
    return out + [f"{ind}}}"]


ROLE_ENUM = {r: f"PORT_ROLE_{r}" for r in ("HOST", "LINK", "ATTACKER", "CPU", "RECIRC")}


def to_textproto(s: Snapshot) -> str:
    lines = ["# proto-file: p4symbex.proto", "# proto-message: symbex.CpSnapshot"]
    lines += [f"# {n}" for n in s.notes]
    lines += [f"program: {_quote(s.program)}", f"target: {_quote(s.target)}",
              f"snapshot_id: {_quote(s.snapshot_id)}", f"captured_at: {_quote(s.captured_at)}"]
    if s.ports:
        lines.append("topology {")
        for p in s.ports:
            extra = "".join(f" {k}: {_quote(v)}" for k, v in (("name", p.name), ("mac", p.mac),
                                                              ("ip", p.ip)) if v)
            lines.append(f"  port {{ port: {p.port} role: {ROLE_ENUM[p.role]}{extra} }}")
        lines.append("}")
    for e in s.entities:
        if s.target == "tofino":
            lines += ["bfrt_entities {"] + _bfrt_entity(e, "  ") + ["}"]
        else:
            lines += ["entities {"] + _p4rt_entity(e, "  ") + ["}"]
    return "\n".join(lines) + "\n"
