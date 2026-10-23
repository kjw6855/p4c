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
    """One table entry, default entry, register cell, fixed-table ($pre/$mirror) entry, or action
    profile member/group ("action_profile_id:<id>", key $MEMBER_ID/$GROUP_ID; a selector table's
    entry refers to one with action "$member"/"$group" and data (("member_id"|"group_id", id),))."""

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
    if e.table.startswith("action_profile_id:"):
        pid = int(e.table.split(":", 1)[1])
        key = e.keys[0]
        if key.name == "$MEMBER_ID":
            params = " ".join(f"params {{ param_name: {_quote(n)} value: {_bytes(int(v))} }}"
                              for n, v in e.data)
            return [f"{ind}action_profile_member {{", f"{i2}action_profile_id: {pid}",
                    f"{i2}member_id: {int(key.value)}",
                    f"{i2}action {{ action_name: {_quote(e.action)} {params} }}", f"{ind}}}"]
        d = dict(e.data)
        out = [f"{ind}action_profile_group {{", f"{i2}action_profile_id: {pid}",
               f"{i2}group_id: {int(key.value)}"]
        out += [f"{i2}members {{ member_id: {int(m)} weight: {int(w)} }}" for m, w in d["members"]]
        if d.get("max_size"):
            out.append(f"{i2}max_size: {int(d['max_size'])}")
        return out + [f"{ind}}}"]
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
    if e.action in ("$member", "$group"):  # an entry of a selector table
        (field, ref), = e.data
        out.append(f"{i2}action {{ action_profile_{field}: {int(ref)} }}")
    else:
        params = " ".join(f"params {{ param_name: {_quote(n)} value: {_bytes(int(v))} }}"
                          for n, v in e.data)
        out.append(f"{i2}action {{ action {{ action_name: {_quote(e.action)} {params} }} }}")
    if e.priority:
        out.append(f"{i2}priority: {int(e.priority)}")
    if e.is_default:
        out.append(f"{i2}is_default_action: true")
    return out + [f"{ind}}}"]


ROLE_ENUM = {r: f"PORT_ROLE_{r}" for r in ("HOST", "LINK", "ATTACKER", "CPU", "RECIRC")}


def _snapshot_fields(s: Snapshot, ind: str) -> List[str]:
    """The fields of a symbex.CpSnapshot message, indented by `ind`."""
    lines = [f"{ind}program: {_quote(s.program)}", f"{ind}target: {_quote(s.target)}",
             f"{ind}snapshot_id: {_quote(s.snapshot_id)}",
             f"{ind}captured_at: {_quote(s.captured_at)}"]
    if s.ports:
        lines.append(f"{ind}topology {{")
        for p in s.ports:
            extra = "".join(f" {k}: {_quote(v)}" for k, v in (("name", p.name), ("mac", p.mac),
                                                              ("ip", p.ip)) if v)
            lines.append(f"{ind}  port {{ port: {p.port} role: {ROLE_ENUM[p.role]}{extra} }}")
        lines.append(f"{ind}}}")
    for e in s.entities:
        if s.target == "tofino":
            lines += [f"{ind}bfrt_entities {{"] + _bfrt_entity(e, ind + "  ") + [f"{ind}}}"]
        else:
            lines += [f"{ind}entities {{"] + _p4rt_entity(e, ind + "  ") + [f"{ind}}}"]
    return lines


def to_textproto(s: Snapshot) -> str:
    lines = ["# proto-file: p4symbex.proto", "# proto-message: symbex.CpSnapshot"]
    lines += [f"# {line}" for n in s.notes for line in str(n).splitlines() or [""]]
    return "\n".join(lines + _snapshot_fields(s, "")) + "\n"


# --- snapshot sequences: init plus the diff each later controller step made ------------------

UPDATE_TYPES = ("INSERT", "MODIFY", "DELETE")


def _cleared(e: Entity) -> Entity:
    """`e` with every data value zeroed: a register cell the dump no longer lists."""
    return Entity(e.table, e.keys, e.action, tuple((n, 0) for n, _ in e.data), e.priority,
                  e.is_default, e.is_register)


def updates(before: List[Entity], after: List[Entity]) -> List[Tuple[str, Entity]]:
    """What changed from `before` to `after`, keyed by Entity.ident(): a new entry is an INSERT, a
    changed one a MODIFY, a vanished one a DELETE. Register cells and default entries always exist,
    so they only change (MODIFY); a register cell that vanishes from the dump reads as zeroed."""
    old = {e.ident(): e for e in before}
    new = {e.ident(): e for e in after}
    out: List[Tuple[str, Entity]] = []
    for e in after:
        o = old.get(e.ident())
        if o is None:
            out.append(("MODIFY" if e.is_register or e.is_default else "INSERT", e))
        elif o != e:
            out.append(("MODIFY", e))
    for e in before:
        if e.ident() not in new:
            out.append(("MODIFY", _cleared(e)) if e.is_register else ("DELETE", e))
    return out


@dataclass
class Step:
    step_id: str
    trigger: str = ""
    updates: List[Tuple[str, Entity]] = field(default_factory=list)


@dataclass
class Sequence:
    """symbex.CpSnapshotSequence: S(0) = init, S(k) = S(k-1) with steps[k-1] applied."""
    init: Snapshot
    scenario: str
    steps: List[Step] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)


def apply_updates(entities: List[Entity], ups: List[Tuple[str, Entity]]) -> List[Entity]:
    """`entities` with `ups` applied in order (a DELETE of a default entry drops it, i.e. the
    table is back to its P4 default)."""
    cur = {e.ident(): e for e in entities}
    for op, e in ups:
        if op == "DELETE":
            if cur.pop(e.ident(), None) is None:
                raise KeyError(f"DELETE of an absent entry: {e}")
        elif op == "INSERT" and e.ident() in cur:
            raise KeyError(f"INSERT of an existing entry: {e}")
        else:
            cur[e.ident()] = e
    return list(cur.values())


def materialize(seq: Sequence, k: int) -> Snapshot:
    """S(k): the init snapshot with steps 1..k applied, as a plain snapshot."""
    if not 0 <= k <= len(seq.steps):
        raise ValueError(f"step {k} outside 0..{len(seq.steps)}")
    ents = list(seq.init.entities)
    for st in seq.steps[:k]:
        ents = apply_updates(ents, st.updates)
    s = seq.init
    return Snapshot(program=s.program, target=s.target, ports=list(s.ports), entities=ents,
                    snapshot_id=f"{seq.scenario}@{k}", captured_at=s.captured_at)


def sequence_to_textproto(seq: Sequence) -> str:
    s = seq.init
    lines = ["# proto-file: p4symbex.proto", "# proto-message: symbex.CpSnapshotSequence"]
    lines += [f"# {line}" for n in seq.notes for line in str(n).splitlines() or [""]]
    lines += [f"program: {_quote(s.program)}", f"target: {_quote(s.target)}",
              f"scenario: {_quote(seq.scenario)}", "init {"]
    lines += _snapshot_fields(s, "  ") + ["}"]
    for st in seq.steps:
        lines += ["steps {", f"  step_id: {_quote(st.step_id)}"]
        if st.trigger:
            lines.append(f"  trigger: {_quote(st.trigger)}")
        for op, e in st.updates:
            lines += ["  updates {", f"    type: CP_UPDATE_{op}"]
            if s.target == "tofino":
                lines += ["    bfrt_entity {"] + _bfrt_entity(e, "      ") + ["    }"]
            else:
                lines += ["    entity {"] + _p4rt_entity(e, "      ") + ["    }"]
            lines.append("  }")
        lines.append("}")
    return "\n".join(lines) + "\n"


# --- textproto reading (no protobuf bindings needed) ------------------------------------------

Msg = Dict[str, list]


def _tokens(text: str):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif c in "{}:":
            yield c
            i += 1
        elif c == '"':
            j, buf = i + 1, bytearray()
            while text[j] != '"':
                if text[j] == "\\":
                    nxt = text[j + 1]
                    if nxt == "x":
                        buf.append(int(text[j + 2:j + 4], 16))
                        j += 4
                        continue
                    if nxt in "01234567":
                        buf.append(int(text[j + 1:j + 4], 8))
                        j += 4
                        continue
                    buf += {"n": b"\n", "t": b"\t", "r": b"\r"}.get(nxt, nxt.encode())
                    j += 2
                else:
                    buf += text[j].encode()
                    j += 1
            yield bytes(buf)
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '{}:"#':
                j += 1
            yield text[i:j]
            i = j


def parse_textproto(text: str) -> Msg:
    """A textproto as {field: [values]}: nested messages are Msg, strings bytes, scalars str."""
    toks = list(_tokens(text))
    pos = 0

    def message(end: Optional[str]) -> Msg:
        nonlocal pos
        out: Msg = {}
        while pos < len(toks) and toks[pos] != end:
            name = toks[pos]
            pos += 1
            if toks[pos] == ":":
                pos += 1
            if toks[pos] == "{":
                pos += 1
                val = message("}")
                pos += 1
            else:
                val = toks[pos]
                pos += 1
            out.setdefault(name, []).append(val)
        return out

    return message(None)


def _one(m: Msg, name: str, default=None):
    v = m.get(name)
    return v[0] if v else default


def _int(b) -> int:
    return int.from_bytes(b, "big") if isinstance(b, bytes) else int(b)


def _str(b, default: str = "") -> str:
    return b.decode() if isinstance(b, bytes) else (default if b is None else str(b))


def _key(m: Msg, name: str) -> Key:
    for kind, second in (("exact", None), ("ternary", "mask"), ("lpm", "prefix_len"),
                         ("range", "high"), ("optional", None)):
        body = _one(m, kind)
        if body is not None:
            value = _int(_one(body, "low" if kind == "range" else "value"))
            return Key(name, kind, value, None if second is None else _int(_one(body, second)))
    raise ValueError(f"key {name}: no match kind")


def _bool(v) -> bool:
    return str(v) == "true"


def entity_from_bfrt(m: Msg) -> Entity:
    keys = tuple(_key(k, _str(_one(k, "field_name"))) for k in m.get("key", []))
    data = []
    for d in m.get("data", []):
        name = _str(_one(d, "field_name"))
        if "str_val" in d:
            data.append((name, _str(_one(d, "str_val"))))
        elif "bool_arr_val" in d:
            data.append((name, [_bool(x) for x in d["bool_arr_val"]]))
        elif "int_arr_val" in d:
            data.append((name, [int(x) for x in d["int_arr_val"]]))
        elif "value" in d:
            data.append((name, _int(_one(d, "value"))))
        else:
            data.append((name, []))  # an empty array: the writer emits no value at all
    is_register = any(k.name == "$REGISTER_INDEX" for k in keys)
    return Entity(_str(_one(m, "table_name")), keys, _str(_one(m, "action_name")), tuple(data),
                  int(_one(m, "priority", 0)), _bool(_one(m, "is_default_entry", "false")),
                  is_register)


def _replicas(m: Msg) -> list:
    return [(_int(_one(r, "port")), int(_one(r, "instance", 0))) for r in m.get("replicas", [])]


def entity_from_p4rt(m: Msg) -> Entity:
    if "register_entry" in m:
        r = m["register_entry"][0]
        idx = int(_one(_one(r, "index"), "index"))
        val = _int(_one(_one(r, "data"), "bitstring"))
        return Entity(f"register_id:{int(_one(r, 'register_id'))}",
                      (Key("$index", "exact", idx),), "", (("value", val),), 0, is_register=True)
    if "packet_replication_engine_entry" in m:
        pre = m["packet_replication_engine_entry"][0]
        if "multicast_group_entry" in pre:
            g = pre["multicast_group_entry"][0]
            return Entity("$multicast_group",
                          (Key("$MGID", "exact", int(_one(g, "multicast_group_id"))),), "",
                          (("replicas", _replicas(g)),), 0)
        c = pre["clone_session_entry"][0]
        return Entity("$clone_session", (Key("$SID", "exact", int(_one(c, "session_id"))),), "",
                      (("replicas", _replicas(c)),), 0)
    if "action_profile_member" in m:
        a = m["action_profile_member"][0]
        act = _one(a, "action", {})
        params = tuple((_str(_one(p, "param_name")), _int(_one(p, "value")))
                       for p in act.get("params", []))
        return Entity(f"action_profile_id:{int(_one(a, 'action_profile_id'))}",
                      (Key("$MEMBER_ID", "exact", int(_one(a, "member_id"))),),
                      _str(_one(act, "action_name")), params, 0)
    if "action_profile_group" in m:
        g = m["action_profile_group"][0]
        members = [(int(_one(x, "member_id")), int(_one(x, "weight", 0)))
                   for x in g.get("members", [])]
        return Entity(f"action_profile_id:{int(_one(g, 'action_profile_id'))}",
                      (Key("$GROUP_ID", "exact", int(_one(g, "group_id"))),), "",
                      (("members", members), ("max_size", int(_one(g, "max_size", 0)))), 0)
    te = m["table_entry"][0]
    keys = tuple(_key(k, _str(_one(k, "field_name"))) for k in te.get("match", []))
    ta = _one(te, "action", {})
    for field in ("member_id", "group_id"):  # an entry of a selector table
        if f"action_profile_{field}" in ta:
            ref = int(_one(ta, f"action_profile_{field}"))
            return Entity(_str(_one(te, "table_name")), keys, "$" + field.split("_")[0],
                          ((field, ref),), int(_one(te, "priority", 0)),
                          _bool(_one(te, "is_default_action", "false")))
    act = _one(ta, "action", {})
    params = tuple((_str(_one(p, "param_name")), _int(_one(p, "value")))
                   for p in act.get("params", []))
    return Entity(_str(_one(te, "table_name")), keys, _str(_one(act, "action_name")), params,
                  int(_one(te, "priority", 0)), _bool(_one(te, "is_default_action", "false")))


def _snapshot_from_msg(m: Msg) -> Snapshot:
    target = _str(_one(m, "target"))
    ports = []
    for p in _one(m, "topology", {}).get("port", []):
        ports.append(Port(int(_one(p, "port")), str(_one(p, "role")).replace("PORT_ROLE_", ""),
                          _str(_one(p, "name")), _str(_one(p, "mac")), _str(_one(p, "ip"))))
    ents = [entity_from_bfrt(e) for e in m.get("bfrt_entities", [])]
    ents += [entity_from_p4rt(e) for e in m.get("entities", [])]
    return Snapshot(program=_str(_one(m, "program")), target=target, ports=ports, entities=ents,
                    snapshot_id=_str(_one(m, "snapshot_id")),
                    captured_at=_str(_one(m, "captured_at")))


def parse_snapshot(text: str) -> Snapshot:
    return _snapshot_from_msg(parse_textproto(text))


def parse_sequence(text: str) -> Sequence:
    m = parse_textproto(text)
    notes = [line[2:] for line in text.splitlines()[2:] if line.startswith("# ")]
    seq = Sequence(init=_snapshot_from_msg(_one(m, "init", {})),
                   scenario=_str(_one(m, "scenario")), notes=notes)
    for st in m.get("steps", []):
        ups = []
        for u in st.get("updates", []):
            op = str(_one(u, "type")).replace("CP_UPDATE_", "")
            if op not in UPDATE_TYPES:
                raise ValueError(f"step {_str(_one(st, 'step_id'))}: update type {op}")
            if "bfrt_entity" in u:
                ups.append((op, entity_from_bfrt(u["bfrt_entity"][0])))
            else:
                ups.append((op, entity_from_p4rt(u["entity"][0])))
        seq.steps.append(Step(_str(_one(st, "step_id")), _str(_one(st, "trigger")), ups))
    return seq
