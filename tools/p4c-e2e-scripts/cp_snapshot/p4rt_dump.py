#!/usr/bin/env python3
"""Dump a P4Runtime server's control-plane state as snapshot entities (PNA-DPDK on infrap4d, or
any P4Runtime target), named through the program's P4Info.

    p4rt_dump.py <p4info .txt|.json|.bin> <grpc_addr> [--device-id N] [--certs DIR]
                 [--server-name NAME]                     (JSON records on stdout)

A TLS server (infrap4d starts in secure mode) needs --certs: a directory with ca.crt and, for
mutual TLS, client.crt and client.key (infrap4d's are in /usr/share/stratum/certs). The address's
host must match the server certificate's name (IPDK's: localhost), or pass --server-name.

Reads, per P4Info table, its entries (wildcard read) and its default entry; every register's cells
(register_entry wildcard; targets that do not serve register reads, such as bmv2, give a note);
and the PRE's multicast groups and clone sessions. Reads need no arbitration, so a running
controller keeps its primary role. Needs grpc and the P4Runtime python bindings (p4.v1), which the
importing host or container provides; capture.py turns the records into snapshot entities
(records_to_entities) the same way bmv2 and BfRt dumps are named.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List, Optional, Tuple

KINDS = ("exact", "ternary", "lpm", "range", "optional")
# Read errors that mean the server is not there (a partial dump would read as deleted entries):
# these abort the dump; any other error is a note (e.g. registers bmv2 does not serve).
UNREACHABLE = ("UNAVAILABLE", "DEADLINE_EXCEEDED")


def _int(b: bytes) -> int:
    return int.from_bytes(b, "big")


def load_p4info(path: Path):
    from google.protobuf import json_format, text_format
    from p4.config.v1 import p4info_pb2
    info = p4info_pb2.P4Info()
    data = path.read_bytes()
    if path.suffix == ".json":
        json_format.Parse(data.decode(), info)
    elif path.suffix == ".bin":
        info.ParseFromString(data)
    else:
        text_format.Parse(data.decode(), info)
    return info


def _key(fm, names) -> list:
    name = names.get(fm.field_id, str(fm.field_id))
    kind = fm.WhichOneof("field_match_type")
    m = getattr(fm, kind)
    if kind == "exact":
        return [name, kind, _int(m.value), None]
    if kind == "ternary":
        return [name, kind, _int(m.value), _int(m.mask)]
    if kind == "lpm":
        return [name, kind, _int(m.value), int(m.prefix_len)]
    if kind == "range":
        return [name, kind, _int(m.low), _int(m.high)]
    return [name, "optional", _int(m.value), None]


def _replicas(entry) -> list:
    out = []
    for r in entry.replicas:
        port = _int(r.port) if r.port else int(getattr(r, "egress_port", 0))
        out.append([port, int(r.instance)])
    return sorted(out)


def channel(addr: str, certs: Optional[Path] = None, server_name: Optional[str] = None):
    """A plaintext channel, or TLS with `certs` (ca.crt; client.crt and client.key for mutual
    TLS)."""
    import grpc
    if not certs:
        return grpc.insecure_channel(addr)
    certs = Path(certs)
    ca = certs / "ca.crt"
    if not ca.exists():
        raise SystemExit(f"TLS: {ca} not found")
    cert, key = certs / "client.crt", certs / "client.key"
    mutual = cert.exists() and key.exists()
    creds = grpc.ssl_channel_credentials(ca.read_bytes(), key.read_bytes() if mutual else None,
                                         cert.read_bytes() if mutual else None)
    opts = [("grpc.ssl_target_name_override", server_name)] if server_name else []
    return grpc.secure_channel(addr, creds, options=opts)


def dump(addr: str, p4info, device_id: int = 0, certs: Optional[Path] = None,
         server_name: Optional[str] = None) -> Tuple[List[dict], List[str]]:
    """(records, notes): one record per table entry, default entry, register cell, multicast group
    and clone session."""
    import grpc
    from p4.v1 import p4runtime_pb2, p4runtime_pb2_grpc

    stub = p4runtime_pb2_grpc.P4RuntimeStub(channel(addr, certs, server_name))
    actions = {a.preamble.id: a for a in p4info.actions}
    notes: List[str] = []

    def read(entity):
        req = p4runtime_pb2.ReadRequest(device_id=device_id)
        req.entities.add().CopyFrom(entity)
        try:
            for resp in stub.Read(req, timeout=60):
                yield from resp.entities
        except grpc.RpcError as ex:
            if ex.code().name in UNREACHABLE:
                hint = ("" if certs else "; a TLS server (infrap4d's default) needs P4RT_CERTS / "
                        "--certs")
                raise SystemExit(f"P4Runtime server {addr}: {ex.code().name} "
                                 f"({ex.details()}){hint}; nothing written") from None
            raise

    def action_of(te):
        if not te.action.HasField("action"):
            return "", []
        a = te.action.action
        info = actions.get(a.action_id)
        pnames = {p.id: p.name for p in info.params} if info else {}
        return (info.preamble.name if info else str(a.action_id),
                [[pnames.get(p.param_id, str(p.param_id)), _int(p.value)] for p in a.params])

    records: List[dict] = []
    table_errors: List[str] = []  # entry reads (not defaults) that failed
    for t in p4info.tables:
        fields = {f.id: f.name for f in t.match_fields}
        for default in (False, True):
            e = p4runtime_pb2.Entity()
            e.table_entry.table_id = t.preamble.id
            e.table_entry.is_default_action = default
            try:
                for ent in read(e):
                    te = ent.table_entry
                    action, params = action_of(te)
                    if default and not action:
                        continue
                    records.append({"table": t.preamble.name,
                                    "keys": [_key(fm, fields) for fm in te.match],
                                    "action": action, "data": params,
                                    "priority": int(te.priority), "is_default": default})
            except grpc.RpcError as ex:
                notes.append(f"read {'default of ' if default else ''}{t.preamble.name}: "
                             f"{ex.code().name}")
                if not default:
                    table_errors.append(f"{ex.code().name} ({ex.details()})")
    if p4info.tables and len(table_errors) == len(p4info.tables):
        # Nothing read at all (a wrong device id, say): the dump would read as every rule deleted.
        raise SystemExit(f"P4Runtime server {addr}: every table read failed, e.g. "
                         f"{table_errors[0]}; check the device id ({device_id}; P4RT_DEVICE_ID) "
                         "and the P4Info; nothing written")
    for r in p4info.registers:
        e = p4runtime_pb2.Entity()
        e.register_entry.register_id = r.preamble.id
        try:
            for ent in read(e):
                re_ = ent.register_entry
                which = re_.data.WhichOneof("data")
                value = _int(re_.data.bitstring) if which == "bitstring" else 0
                records.append({"table": f"register_id:{r.preamble.id}",
                                "keys": [["$index", "exact", int(re_.index.index), None]],
                                "action": "", "data": [["value", value]], "priority": 0,
                                "is_register": True})
        except grpc.RpcError as ex:
            notes.append(f"read register {r.preamble.name}: {ex.code().name}")
    for kind, table, key in (("multicast_group_entry", "$multicast_group", "$MGID"),
                             ("clone_session_entry", "$clone_session", "$SID")):
        e = p4runtime_pb2.Entity()
        getattr(e.packet_replication_engine_entry, kind).SetInParent()
        try:
            for ent in read(e):
                g = getattr(ent.packet_replication_engine_entry, kind)
                gid = g.multicast_group_id if kind == "multicast_group_entry" else g.session_id
                records.append({"table": table, "keys": [[key, "exact", int(gid), None]],
                                "action": "", "data": [["replicas", _replicas(g)]],
                                "priority": 0})
        except grpc.RpcError as ex:
            notes.append(f"read {kind}: {ex.code().name}")
    return records, notes


def records_to_entities(records: List[dict]):
    """dump() records -> snapshot entities (needs no P4Runtime bindings)."""
    from cp_snapshot.snapshot import Entity, Key
    out = []
    for r in records:
        data = tuple((n, [tuple(x) for x in v] if n == "replicas" else v) for n, v in r["data"])
        out.append(Entity(r["table"], tuple(Key(*k) for k in r["keys"]), r["action"], data,
                          r.get("priority", 0), r.get("is_default", False),
                          r.get("is_register", False)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("p4info", type=Path)
    ap.add_argument("addr")
    ap.add_argument("--device-id", type=int, default=0)
    ap.add_argument("--certs", type=Path, help="TLS: directory with ca.crt[, client.crt, client.key]")
    ap.add_argument("--server-name", help="TLS: the name the server certificate is checked against")
    a = ap.parse_args()
    records, notes = dump(a.addr, load_p4info(a.p4info), a.device_id, a.certs, a.server_name)
    json.dump({"records": records, "notes": notes}, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
