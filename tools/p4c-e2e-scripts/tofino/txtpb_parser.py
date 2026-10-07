"""Parser for the BfRt-flavoured tampering .txtpb files emitted by p4symbex's
``--test-backend BFRT`` backend.

The grammar follows ``targets/tofino/proto/p4symbex_bfrt.proto``:

  input_packet { packet: "…" port: N }
  expected_output_packet { packet: "…" port: N packet_mask: "…" }
  entities {
    table_name: "…"
    action_name: "…"
    priority: N
    phase: N           # 1 or 2 (3 doesn't install new entries)
    key   { field_name: "…" exact|ternary|lpm|range|optional { … } }
    data  { field_name: "…" value: "…" }
  }
  affected_register {
    register_name: "…"
    index: N
    attacker_value: "…"
    sink_table: "table_a,table_b"   # optional; Key-sink chains only
    hit_phase: 1                    # optional; HIT/MISS kinds only (see below)
    miss_phase: 3                   # optional; HIT/MISS kinds only (see below)
    sink_outcome_legit: "…"         # optional; report-only outcome pair
    sink_outcome_attack: "…"        # optional; report-only outcome pair
    match_kind: REGISTER_MATCH_AT_LEAST   # optional; default EXACT
    min_value: "…"                  # present only with AT_LEAST
  }

``hit_phase`` / ``miss_phase`` are triage metadata, not a replay criterion: the
oracle is the legit-vs-attack Phase-3 differential in :mod:`tofino.packet_tester`,
which never asks about hit/miss. The generator emits the pair only for the two
HIT/MISS tamper kinds; an action-divergence case reaches the sink in BOTH runs
and so carries neither (it names its two outcomes in ``sink_outcome_*``
instead). Both fields therefore default to 0 = "no claim", and nothing reads
them.

This module is target-agnostic with respect to runtime — it does not touch
bfrt_grpc; that's :mod:`tofino.bfrt_grpc_client`'s job.
"""

from __future__ import annotations

import codecs
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional, Tuple


@dataclass
class KeyField:
    field_name: str
    match_kind: str          # exact | ternary | lpm | range | optional
    value: object            # int (from "0x…") or bytearray; range low
    secondary: Optional[object] = None  # mask | prefix_len | range high


@dataclass
class DataField:
    field_name: str
    value: object  # int (from hex "0x…") or bytearray (from escaped binary)


@dataclass
class BfRtEntity:
    table_name: str
    action_name: Optional[str]
    keys: List[KeyField] = field(default_factory=list)
    data: List[DataField] = field(default_factory=list)
    priority: int = 0
    phase: int = 0   # 0 = untagged (single-phase tests); 1 or 2 for tampering
    # True for a table's DEFAULT entry (the action a controller installs on a keyless table) rather
    # than a match entry. The driver must then call default_entry_set, which takes data only, and
    # `keys` is empty. An explicit flag, not "keys == []": a keyless MATCH table also has no key
    # fields, so emptiness alone cannot tell the two apart.
    is_default_entry: bool = False


@dataclass
class Phase:
    in_packet: bytes = b""
    in_port: int = 0
    exp_packet: Optional[bytes] = None
    exp_port: Optional[int] = None
    exp_mask: Optional[bytes] = None
    # True for a Phase-2 (tamper) packet: sent only in the tamper run, not the legit run. Multiple
    # consecutive tamper_only packets form a multi-packet Phase 2 (accumulation).
    tamper_only: bool = False
    # Number of times to send this packet (default 1). Large-k accumulation uses one block with
    # repeat_count=k instead of k literal blocks.
    repeat_count: int = 1


@dataclass
class AffectedRegister:
    register_name: str
    index: int
    # The value the register should hold AFTER Phase 2 (before Phase 3 replays).
    attacker_value: bytes
    sink_tables: List[str] = field(default_factory=list)
    # Triage metadata only; 0 = the file made no HIT/MISS claim (an action-divergence case, or a
    # file older than the fields). No replay decision reads these -- see the module docstring.
    hit_phase: int = 0
    miss_phase: int = 0
    # How to compare what we read against the expectation. "" / absent => exact,
    # so files generated before the field existed keep their original meaning.
    match_kind: str = "REGISTER_MATCH_EXACT"
    # Lower bound, set only for AT_LEAST: the value at which the sink flips. An
    # accumulating register has no single observable value (the flood stops as
    # soon as it reaches its target, and packet loss changes where that lands),
    # so crossing this bound is the real success condition.
    min_value: bytes = b""

    @property
    def at_least(self) -> bool:
        return self.match_kind == "REGISTER_MATCH_AT_LEAST" and bool(self.min_value)


@dataclass
class MulticastGroup:
    """A multicast group the harness must install before replay (and remove after).

    Emitted only when the forwarding path is multicast. p4symbex models multicast as a
    single representative egress port, so ``replica_ports`` is typically one port.
    """
    mgid: int
    replica_ports: List[int] = field(default_factory=list)


@dataclass
class TamperCase:
    phases: List[Phase]
    entities: List[BfRtEntity]
    affected_registers: List[AffectedRegister]
    path: str
    multicast_groups: List[MulticastGroup] = field(default_factory=list)

    @property
    def tamper_indices(self) -> List[int]:
        """Indices of the Phase-2 (tamper_only) packets — sent only in the tamper run."""
        return [i for i, ph in enumerate(self.phases) if ph.tamper_only]

    @property
    def legit_indices(self) -> List[int]:
        """Indices replayed in the legit run: every non-tamper packet (Phase 1 + Phase 3)."""
        return [i for i, ph in enumerate(self.phases) if not ph.tamper_only]

    @property
    def attack_indices(self) -> List[int]:
        """Indices replayed in the tamper run: all packets, in order."""
        return list(range(len(self.phases)))


# ----------------------- low-level textproto walker ----------------------- #

_KV_STR_RE = re.compile(r'(\w+)\s*:\s*"((?:[^"\\]|\\.)*)"')
_KV_NUM_RE = re.compile(r'(\w+)\s*:\s*(-?\d+)')


def _decode_escaped(s: str) -> bytes:
    return codecs.escape_decode(s.encode("latin-1"))[0]


def _parse_entity_value(s: str):
    """Parse a value from an entity key/data block.

    Entity values in p4symbex BFRT txtpb are always hex integers ("0x…").
    bfrt_grpc KeyTuple / DataTuple require int, bytearray, or str — NOT bytes.
    Returning int satisfies all match kinds (exact, ternary, lpm, range, optional).
    """
    stripped = s.strip()
    if stripped.startswith(("0x", "0X")):
        return int(stripped, 16)
    # Fallback: binary-escaped value → bytearray (not bytes) for bfrt_grpc compat.
    return bytearray(codecs.escape_decode(stripped.encode("latin-1"))[0])


def _iter_top_level_blocks(text: str) -> Iterable[Tuple[str, str]]:
    """Yield (header, body) for every top-level `header { ... }` block."""
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        if text[i] == "#":
            while i < n and text[i] != "\n":
                i += 1
            continue
        m = re.match(r"(\w+)\s*", text[i:])
        if not m:
            i += 1
            continue
        header = m.group(1)
        j = i + m.end()
        if j < n and text[j] == "{":
            depth, j = 1, j + 1
            start = j
            while j < n and depth > 0:
                c = text[j]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                elif c == '"':
                    j += 1
                    while j < n:
                        if text[j] == "\\":
                            j += 2
                            continue
                        if text[j] == '"':
                            break
                        j += 1
                j += 1
            yield header, text[start:j - 1]
            i = j
        else:
            while i < n and text[i] != "\n":
                i += 1


def _str_field(body: str, key: str, default: str = "") -> str:
    for k, v in _KV_STR_RE.findall(body):
        if k == key:
            return v
    return default


def _enum_field(body: str, key: str, default: str = "") -> str:
    """Read a proto-text enum: a bare identifier, not a quoted string, so _str_field misses it."""
    m = re.search(rf"\b{key}\s*:\s*([A-Za-z_][A-Za-z0-9_]*)", body)
    return m.group(1) if m else default


def _int_field(body: str, key: str, default: int = 0) -> int:
    for k, v in _KV_NUM_RE.findall(body):
        if k == key:
            return int(v)
    return default


def _bool_field(body: str, key: str, default: bool = False) -> bool:
    m = re.search(rf"\b{re.escape(key)}\s*:\s*(true|false)\b", body)
    if m:
        return m.group(1) == "true"
    return default


# ----------------------- block-level parsers ----------------------- #

def _parse_packet_block(body: str) -> Tuple[bytes, int, Optional[bytes]]:
    pkt = _decode_escaped(_str_field(body, "packet"))
    port = _int_field(body, "port", 0)
    mask_raw = _str_field(body, "packet_mask", "")
    mask = _decode_escaped(mask_raw) if mask_raw else None
    return pkt, port, mask


def _parse_entity_block(body: str) -> BfRtEntity:
    ent = BfRtEntity(
        table_name=_str_field(body, "table_name"),
        action_name=_str_field(body, "action_name") or None,
        priority=_int_field(body, "priority", 0),
        phase=_int_field(body, "phase", 0),
        is_default_entry=_bool_field(body, "is_default_entry"),
    )
    for hdr, sub in _iter_top_level_blocks(body):
        if hdr == "key":
            kf = _parse_key_field(sub)
            if kf is not None:
                ent.keys.append(kf)
        elif hdr == "data":
            df = DataField(field_name=_str_field(sub, "field_name"),
                           value=_parse_entity_value(_str_field(sub, "value")))
            ent.data.append(df)
    return ent


def _parse_key_field(body: str) -> Optional[KeyField]:
    fname = _str_field(body, "field_name")
    for hdr, sub in _iter_top_level_blocks(body):
        if hdr == "exact":
            return KeyField(fname, "exact", _parse_entity_value(_str_field(sub, "value")))
        if hdr == "ternary":
            return KeyField(fname, "ternary",
                            _parse_entity_value(_str_field(sub, "value")),
                            secondary=_parse_entity_value(_str_field(sub, "mask")))
        if hdr == "lpm":
            return KeyField(fname, "lpm",
                            _parse_entity_value(_str_field(sub, "value")),
                            secondary=_int_field(sub, "prefix_len", 0))
        if hdr == "range":
            return KeyField(fname, "range",
                            _parse_entity_value(_str_field(sub, "low")),
                            secondary=_parse_entity_value(_str_field(sub, "high")))
        if hdr == "optional":
            return KeyField(fname, "optional", _parse_entity_value(_str_field(sub, "value")))
    return None


def _parse_multicast_group(body: str) -> MulticastGroup:
    mgid = _int_field(body, "mgid", 0)
    ports = [int(v) for k, v in _KV_NUM_RE.findall(body) if k == "replica_port"]
    return MulticastGroup(mgid=mgid, replica_ports=ports)


def _parse_affected_register(body: str) -> AffectedRegister:
    return AffectedRegister(
        register_name=_str_field(body, "register_name"),
        index=_int_field(body, "index", 0),
        attacker_value=_decode_escaped(_str_field(body, "attacker_value")),
        sink_tables=[t.strip() for t in _str_field(body, "sink_table").split(",") if t.strip()],
        hit_phase=_int_field(body, "hit_phase", 0),
        miss_phase=_int_field(body, "miss_phase", 0),
        match_kind=(_enum_field(body, "match_kind") or "REGISTER_MATCH_EXACT"),
        min_value=_decode_escaped(_str_field(body, "min_value")),
    )


# ----------------------- public entry point ----------------------- #

def parse_tampering_txtpb(path: Path) -> TamperCase:
    """Parse a 3-phase BFRT tampering .txtpb file."""
    text = path.read_text()
    phases: List[Phase] = []
    entities: List[BfRtEntity] = []
    regs: List[AffectedRegister] = []
    mcast_groups: List[MulticastGroup] = []
    current: Optional[Phase] = None

    for header, body in _iter_top_level_blocks(text):
        if header == "input_packet":
            if current is not None:
                phases.append(current)
            pkt, port, _ = _parse_packet_block(body)
            current = Phase(in_packet=pkt, in_port=port,
                            tamper_only=_bool_field(body, "tamper_only"),
                            repeat_count=_int_field(body, "repeat_count", 1))
        elif header == "expected_output_packet":
            if current is None:
                raise ValueError(f"{path}: expected_output_packet without input_packet")
            pkt, port, mask = _parse_packet_block(body)
            current.exp_packet = pkt
            current.exp_port = port
            current.exp_mask = mask if mask is not None else b"\xFF" * len(pkt)
        elif header == "entities":
            entities.append(_parse_entity_block(body))
        elif header == "affected_register":
            regs.append(_parse_affected_register(body))
        elif header == "multicast_group":
            mcast_groups.append(_parse_multicast_group(body))

    if current is not None:
        phases.append(current)
    # A well-formed tampering file has Phase 1, one or more Phase-2 (tamper_only) packets, and
    # Phase 3 — at least three packets. Pad only if malformed (downstream detects empty phases).
    # Do NOT truncate: a multi-packet Phase 2 legitimately has more than three input_packet blocks.
    while len(phases) < 3:
        phases.append(Phase())
    return TamperCase(phases=phases, entities=entities,
                      affected_registers=regs, path=str(path),
                      multicast_groups=mcast_groups)
