"""Dump a running tofino-model/bf_switchd pipeline's control-plane state over BF-Runtime gRPC.

Reads every table of the bound program: match-action entries and default entries, register cells,
run-time register parameters, and the fixed $pre/$mirror tables. Values are returned as plain ints
(or int/bool lists) in `snapshot.Entity` form; the capture keeps only what the controller changed.
"""

from __future__ import annotations

import ipaddress
import logging
import os
import sys
from typing import Any, Dict, Iterable, List, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from cp_snapshot.snapshot import Entity, Key, Value  # noqa: E402
from tofino import bfrt_grpc_client  # noqa: E402

log = logging.getLogger("cp_snapshot.dump_bfrt")

MATCH_TYPES = {"MatchAction_Direct", "MatchAction_Indirect"}
# Fixed tables whose state a controller sets and replay needs ($pre: multicast; $mirror: sessions).
FIXED_TABLES = ("$pre.mgid", "$pre.node", "$mirror.cfg")
# Direct counter / idle-time state of a match-action entry: traffic statistics, not rules.
DIRECT_STATE = ("$COUNTER_SPEC_PKTS", "$COUNTER_SPEC_BYTES", "$ENTRY_HIT_STATE", "$ENTRY_TTL")


def _to_int(v: Any) -> Value:
    """bfrt_grpc returns ints, bools, MAC/IP strings, byte strings or per-pipe lists."""
    if isinstance(v, bool):
        return v
    if isinstance(v, int):
        return v
    if isinstance(v, (bytes, bytearray)):
        return int.from_bytes(v, "big")
    if isinstance(v, str):
        s = v.strip()
        if ":" in s and all(len(p) <= 2 for p in s.split(":")):  # MAC
            return int(s.replace(":", ""), 16)
        try:
            return int(ipaddress.ip_address(s))
        except ValueError:
            pass
        try:
            return int(s, 0)
        except ValueError:
            return s
    if isinstance(v, (list, tuple)):
        return [_to_int(x) for x in v]
    return str(v)


def _keys(key_dict: Dict[str, Dict[str, Any]]) -> Tuple[Tuple[Key, ...], int]:
    keys: List[Key] = []
    priority = 0
    for name, spec in sorted(key_dict.items()):
        if name == "$MATCH_PRIORITY":
            priority = int(_to_int(spec.get("value", 0)))
            continue
        if "mask" in spec:
            keys.append(Key(name, "ternary", _to_int(spec["value"]), _to_int(spec["mask"])))
        elif "prefix_len" in spec:
            keys.append(Key(name, "lpm", _to_int(spec["value"]), int(spec["prefix_len"])))
        elif "low" in spec:
            keys.append(Key(name, "range", _to_int(spec["low"]), _to_int(spec["high"])))
        elif "is_valid" in spec:  # optional match
            if spec.get("is_valid"):
                keys.append(Key(name, "optional", _to_int(spec["value"])))
        else:
            keys.append(Key(name, "exact", _to_int(spec["value"])))
    return tuple(keys), priority


def _data(data_dict: Dict[str, Any], skip: Iterable[str] = ()) -> Tuple[str, Tuple]:
    action = data_dict.get("action_name") or ""
    fields = []
    for name, v in sorted(data_dict.items()):
        if name in ("action_name", "is_default_entry") or name in skip:
            continue
        fields.append((name, _to_int(v)))
    return action, tuple(fields)


def _collapse_pipes(v: Value) -> Value:
    """Register reads return one value per pipe; keep one when all pipes agree."""
    if isinstance(v, list) and v and not isinstance(v[0], list):
        if len(set(v)) > 1:
            log.warning("register cell differs across pipes %s; using pipe 0", v)
        return v[0]
    return v


def _register_cells(table, target, from_hw: bool) -> List[Entity]:
    """All cells of a Register table, read from the raw ReadResponse.

    bfrt_grpc's entry_get builds presentation dicts per cell (~1 ms each); programs here have up
    to ~11M cells, so this reads the protobuf fields directly: the $REGISTER_INDEX key and each
    data field's per-pipe int array (or byte stream)."""
    from bfrt_grpc import bfruntime_pb2, client as gc  # noqa: PLC0415 - SDE path set by caller
    req = bfruntime_pb2.ReadRequest()
    gc._cpy_target(req, target)
    table._entry_read_req_make(req, None, {"from_hw": from_hw}, None, False)
    name = table.info.name_get()
    names: Dict[int, str] = {}
    out: List[Entity] = []
    for rep in table.reader_writer_interface._read(req):
        for entity in rep.entities:
            te = entity.table_entry
            if te.is_default_entry or not te.HasField("key"):
                continue
            idx = int.from_bytes(te.key.fields[0].exact.value, "big")
            # One value per pipe: either an int array or the field repeated once per pipe.
            values: Dict[str, List[int]] = {}
            for f in te.data.fields:
                fname = names.get(f.field_id)
                if fname is None:
                    fname = names[f.field_id] = table.info.data_field_name_get(f.field_id)
                if fname.startswith("$"):
                    continue
                if f.HasField("int_arr_val"):
                    values.setdefault(fname, []).extend(int(x) for x in f.int_arr_val.val)
                elif f.HasField("stream"):
                    values.setdefault(fname, []).append(int.from_bytes(f.stream, "big"))
            fields = tuple(sorted((n, _collapse_pipes(v)) for n, v in values.items()))
            out.append(Entity(name, (Key("$REGISTER_INDEX", "exact", idx),), "", fields, 0,
                              is_register=True))
    return out


def dump(client: "bfrt_grpc_client.BfRtClient") -> Tuple[List[Entity], List[str]]:
    """All entities of the bound program, plus notes on what was skipped."""
    target = client.target
    entities: List[Entity] = []
    notes: List[str] = []
    seen = set()
    for _, table in client.bfrt_info.table_dict.items():
        name = table.info.name_get()
        if name in seen or name.endswith(".idle"):
            continue
        seen.add(name)
        try:
            ttype = table.info.type_get()
        except Exception:  # noqa: BLE001
            continue
        try:
            if ttype in MATCH_TYPES:
                for data, key in table.entry_get(target, [], {"from_hw": False}):
                    keys, prio = _keys(key.to_dict())
                    action, fields = _data(data.to_dict(), skip=DIRECT_STATE)
                    entities.append(Entity(name, keys, action, fields, prio))
                for data, _key in table.default_entry_get(target):
                    action, fields = _data(data.to_dict(), skip=DIRECT_STATE)
                    entities.append(Entity(name, (), action, fields, 0, is_default=True))
            elif ttype == "Register" and os.environ.get("DUMP_BFRT_SLOW_REGISTERS") != "1":
                # The driver's shadow, not the device: a capture runs no traffic, so only the
                # control plane writes registers and the shadow equals the device (checked on
                # FANcY: identical). Reading the model's hardware takes ~1 ms per cell.
                from_hw = os.environ.get("DUMP_BFRT_FROM_HW", "0") == "1"
                try:
                    entities.extend(_register_cells(table, target, from_hw))
                except Exception as e:  # noqa: BLE001 - some registers serve no shadow read
                    if from_hw:
                        raise
                    notes.append(f"{name}: shadow read failed ({type(e).__name__}); "
                                 "read from the device")
                    entities.extend(_register_cells(table, target, True))
            elif ttype == "Register":
                for data, key in table.entry_get(target, [], {"from_hw": True}):
                    idx = int(key.to_dict()["$REGISTER_INDEX"]["value"])
                    fields = tuple((n, _collapse_pipes(_to_int(v)))
                                   for n, v in sorted(data.to_dict().items())
                                   if not n.startswith("$")
                                   and n not in ("is_default_entry", "action_name"))
                    entities.append(Entity(name, (Key("$REGISTER_INDEX", "exact", idx),), "",
                                           fields, 0, is_register=True))
            elif ttype == "RegisterParam":
                for data, _key in table.default_entry_get(target):
                    _, fields = _data(data.to_dict())
                    entities.append(Entity(name, (), "", fields[:1], 0))
            elif ttype == "PortMetadata":  # the parser's per-port metadata ($PORT_METADATA)
                for data, key in table.entry_get(target, [], {"from_hw": False}):
                    keys, _ = _keys(key.to_dict())
                    _, fields = _data(data.to_dict())
                    entities.append(Entity(name, keys, "", fields, 0))
            elif name in FIXED_TABLES:
                for data, key in table.entry_get(target, [], {"from_hw": False}):
                    keys, _ = _keys(key.to_dict())
                    action, fields = _data(data.to_dict())
                    entities.append(Entity(name, keys, action, fields, 0))
            elif ttype in ("ActionProfile", "Selector", "SelectorGetMember",
                           "MatchAction_Indirect_Selector"):
                notes.append(f"{name}: {ttype} not captured (action profiles are unsupported)")
        except Exception as e:  # noqa: BLE001 - one unreadable table must not lose the rest
            notes.append(f"{name}: read failed ({' '.join(str(e).split())[:300]})")
            log.warning("dump %s (%s) failed: %s", name, ttype, e)
    return entities, notes
