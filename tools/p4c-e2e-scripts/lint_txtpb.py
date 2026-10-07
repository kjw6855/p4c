#!/usr/bin/env python3
"""Validate a tampering .txtpb (BFRT or BMv2 protobuf flavour) against the
post-generation soundness invariants.

Checks (each has a name usable with --disable / --only):

  sink-key-collision
      For every affected_register block that carries `sink_table` / `hit_phase`
      / `miss_phase` metadata, assert the `attacker_value` is not one of the
      match-key values installed on the named sink table -- otherwise the
      Phase-3 lookup would still HIT, violating the "Phase 1 HIT, Phase 3 MISS"
      invariant. Skipped for action-divergence (`adiv`) cases: those reach the
      sink in BOTH runs and diverge by running a different installed entry, so
      a collision is what the case is FOR, not a defect. Such a case emits no
      `hit_phase` / `miss_phase` at all, which is the second, independent way
      this check recognises that no HIT/MISS claim is being made.

  hash-index-default
      `affected_register.index` must not be the unconstrained solver default
      (0) when the register is indexed through a hash. An unresolved concolic
      hash variable models as Z3's default for its label, so a hash-indexed
      register reporting cell 0 is almost always "nothing pinned the index",
      not "the CRC really lands on cell 0".

  hash-input-phase-mismatch
      The Phase-2 write must hash the same input as the Phase-1 read, or the
      attacker writes a different cell than the victim reads and the case
      cannot fire. "Same input" means the same operand *values*, not the same
      field names: Phase-2 index steering deliberately makes the attacker's
      own field carry the victim's value, so two different fields holding the
      same value are the same cell. Both the field a packet variable came from
      and its solved value are recovered per phase from the trace (see
      `_pktvar_field_map`), so the comparison stays correct even when the two
      phases take different parse paths.

  cp-arg-phase-mismatch
      Phase 1 and Phase 2 must agree on every control-plane action argument
      they share. Each phase is a separate solver query over symbols that are
      only scoped by table/action/parameter name, so the phases can silently
      model the same entry with different action data. Two independent
      sources are cross-checked: the serialized `entities` (keyed tables, and
      since p4symbex learned to emit them, default entries too) and the
      symbolic action arguments recovered from the traces (still the only view
      onto a default-action table whose entry the generator did not serialize,
      e.g. output produced before that change).

Everything is derived from the file itself -- register names, hash operands,
packet-variable-to-field mapping and action arguments all come out of the
traces and the emitted entities, so no program-specific knowledge is baked in
and both protobuf flavours go through the same code.

Exit code 0 = all .txtpb files pass; non-zero = at least one violation
(a line per failing file printed to stderr).

Usage:  python3 lint_txtpb.py [--verbose] [--disable CHECK] path/to/*.txtpb
"""

from __future__ import annotations

import codecs
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


_KV_RE = re.compile(r'(\w+)\s*:\s*"((?:[^"\\]|\\.)*)"')   # foo: "..."
_NUM_RE = re.compile(r'(\w+)\s*:\s*(-?\d+)')              # foo: 123

# `traces: '[P1] ...'` -- one trace event per line, phase-tagged by the emitting template.
_TRACE_RE = re.compile(r"^\s*traces:\s*'\[P(?P<phase>\d+)\]\s?(?P<body>.*)'\s*$")

# Every entities block is preceded by `# Table <name> (Phase <n>)` in both templates; the BFRT
# flavour repeats the phase as a real field, the BMv2 one does not, so the comment is the only
# portable carrier.
_ENTITY_HEAD_RE = re.compile(
    r"^(?:#[ \t]*Table[ \t]+(?P<table>.*?)[ \t]*\(Phase[ \t]*(?P<phase>\d+)\)[ \t]*\r?\n)?"
    r"entities[ \t]*\{",
    re.M)

# `[ExtractSuccess] hdr.eth@0 | Condition: ... | Extract Size: 112 -> f = v | f = v`
_EXTRACT_HEAD_RE = re.compile(r"\[ExtractSuccess\]\s+(?P<hdr>\S+)@(?P<off>-?\d+)\s")

# A control-plane action argument symbol: `|<table>_<action>_arg_<param>[_<n>](<type>)|`.
# The name carries dots (fully qualified table/action names) but never `|` or `(`.
_CP_ARG_RE = re.compile(r"\|(?P<name>[^|(]*_arg_[^|(]*)\((?P<type>[^)|]*)\)\|")

# `[AssignmentStatement]: <lhs> = <rhs>;| Computed: <lhs> = <value>;`
_ASSIGN_RE = re.compile(
    r"\[AssignmentStatement\]:\s*(?P<lhs>.*?)\s*=\s*(?P<rhs>.*?);\s*\|\s*Computed:\s*"
    r"(?P<clhs>.*?)\s*=\s*(?P<value>.*?);?\s*$")

_PKTVAR_RE = re.compile(r"\|(?P<name>pktvar_\d+)\((?P<type>[^)|]*)\)\|")

# A concolic hash placeholder, e.g. `Concolic_Hash_get_1627040_0(...)` (Tofino) or
# `Concolic_*method_hash_1131829_0(...)` (BMv2). Both targets register their hash externs as
# concolic methods, so the label is the portable "this index came out of a hash" marker.
_CONCOLIC_HASH_RE = re.compile(r"Concolic_[^(\s]*hash[^(\s]*", re.I)

# What the generator claims the attacker changed at the sink (p4symbex `TamperKind`). Every emitted
# file states it twice: as the label half of the `Tamper case:` metadata line (`tamperKindLabel`)
# and as the `_<tag>` suffix both tampering back ends append to the file name (`tamperKindTag`, in
# targets/*/test_backend/{bfrt,protobuf}.cpp). The metadata is read first because it survives a file
# being renamed or copied out of its run directory; the name is the fallback.
_CASE_KIND_TAGS = {
    "HIT_TO_MISS": "h2m",
    "MISS_TO_HIT": "m2h",
    "ACTION_DIVERGE": "adiv",
    "COND_TRUE_TO_FALSE": "c_t2f",
    "COND_FALSE_TO_TRUE": "c_f2t",
}

# `metadata: "Tamper case: ACTION_DIVERGE/FWD_TO_FWD"` -- the label, then a `/` and a disposition.
_TAMPER_CASE_RE = re.compile(r'metadata:\s*"Tamper case:\s*(?P<label>[A-Z_]+)')

CHECKS = (
    "sink-key-collision",
    "hash-index-default",
    "hash-input-phase-mismatch",
    "cp-arg-phase-mismatch",
)


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


def _decode(s: str) -> bytes:
    return codecs.escape_decode(s.encode("latin-1"))[0]


def _parse_kv_str(body: str, key: str) -> str:
    for k, v in _KV_RE.findall(body):
        if k == key:
            return v
    return ""


def _parse_kv_int(body: str, key: str) -> int:
    for k, v in _NUM_RE.findall(body):
        if k == key:
            return int(v)
    return -1


def _parse_kv_bool(body: str, *keys: str) -> bool:
    """True when any of @keys is present and set. Takes several names because the same flag is
    spelled `is_default_entry` in the BFRT flavour and `is_default_action` in the BMv2 one."""
    for key in keys:
        if re.search(rf"^\s*{re.escape(key)}\s*:\s*true\b", body, re.MULTILINE):
            return True
    return False


def _norm_scalar(raw: str) -> str:
    """Normalise a control-plane value to a comparable canonical string.

    The two flavours print the same number three ways (`0x1FF`, `\\x01\\xFF`,
    `511`) and traces print booleans as `true`/`false`, so canonicalise to a
    decimal integer whenever the value is numeric and leave anything else
    (taint markers, struct literals) as its stripped text.
    """
    s = raw.strip().rstrip(";").strip()
    if s in ("true", "True"):
        return "1"
    if s in ("false", "False"):
        return "0"
    try:
        if s.lower().startswith("0x"):
            return str(int(s, 16))
        return str(int(s, 10))
    except ValueError:
        pass
    if "\\x" in s:
        try:
            return str(int.from_bytes(_decode(s), "big"))
        except Exception:      # noqa: BLE001 - malformed escapes stay opaque text
            return s
    return s


def _split_top_level(s: str, sep: str = ",") -> List[str]:
    """Split on `sep` at bracket depth zero, ignoring quoted spans."""
    out, depth, start, in_str = [], 0, 0, False
    i = 0
    while i < len(s):
        c = s[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(s[start:i])
            start = i + 1
        i += 1
    out.append(s[start:])
    return [p.strip() for p in out]


def _balanced_call_args(body: str, callee: str) -> List[List[str]]:
    """Return the top-level argument lists of every `callee(...)` in `body`."""
    calls: List[List[str]] = []
    for m in re.finditer(re.escape(callee) + r"\(", body):
        depth, j = 1, m.end()
        while j < len(body) and depth > 0:
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
            j += 1
        if depth == 0:
            calls.append(_split_top_level(body[m.end():j - 1]))
    return calls


# ---------------------------------------------------------------------------
# Parsing: entities (both flavours) and traces
# ---------------------------------------------------------------------------

class Entity:
    """One serialized control-plane entry, flattened across both flavours."""

    def __init__(self, phase: int, table: str, action: str,
                 keys: List[Tuple[str, str]], data: List[Tuple[str, str]],
                 is_default: bool = False):
        self.phase = phase
        self.table = table
        self.action = action
        self.keys = keys      # ordered [(field_name, normalised value)]
        self.data = data      # ordered [(param name,  normalised value)]
        # A table's DEFAULT entry rather than a match entry. Spelled `is_default_entry` in the BFRT
        # flavour and `is_default_action` (the native P4Runtime field) in the BMv2 one.
        self.is_default = is_default

    @property
    def key_signature(self) -> Tuple[Tuple[str, str], ...]:
        return tuple(sorted(self.keys))

    @property
    def entry_signature(self) -> Tuple:
        """Identity of the physical entry, for grouping the same entry across phases.

        The default flag belongs here: a default entry carries no keys, so on a table that also has
        a keyless match entry the two would share a key_signature and be compared as if they were
        the same entry."""
        return (self.key_signature, self.is_default)


def _parse_entities(text: str) -> List[Entity]:
    """Flatten every `entities { ... }` block of either flavour into `Entity`."""
    # The block bodies and the `# Table X (Phase N)` comments appear in the same order, so pair
    # them positionally; the BFRT `phase:` field wins where it exists.
    comment_phases = [int(m.group("phase")) if m.group("phase") else -1
                      for m in _ENTITY_HEAD_RE.finditer(text)]
    entities: List[Entity] = []
    ordinal = 0
    for hdr, body in _iter_top_level_blocks(text):
        if hdr != "entities":
            continue
        phase = comment_phases[ordinal] if ordinal < len(comment_phases) else -1
        ordinal += 1

        # BMv2 wraps everything one level deeper in `table_entry { ... }`.
        inner = body
        for sub_hdr, sub_body in _iter_top_level_blocks(body):
            if sub_hdr == "table_entry":
                inner = sub_body
                break

        table = _parse_kv_str(inner, "table_name")
        action = _parse_kv_str(inner, "action_name")
        explicit_phase = _parse_kv_int(inner, "phase")
        if explicit_phase >= 0:
            phase = explicit_phase

        keys: List[Tuple[str, str]] = []
        data: List[Tuple[str, str]] = []
        for sub_hdr, sub_body in _iter_top_level_blocks(inner):
            if sub_hdr in ("key", "match"):
                fname = _parse_kv_str(sub_body, "field_name")
                # The match kind (exact/ternary/lpm/...) is one nested block deep in both flavours.
                val = _parse_kv_str(sub_body, "value")
                if not val:
                    for _kind, kind_body in _iter_top_level_blocks(sub_body):
                        val = _parse_kv_str(kind_body, "value")
                        if val:
                            break
                keys.append((fname, _norm_scalar(val)))
            elif sub_hdr == "data":
                data.append((_parse_kv_str(sub_body, "field_name"),
                             _norm_scalar(_parse_kv_str(sub_body, "value"))))
            elif sub_hdr == "action":
                # BMv2: action { action { action_name, params { param_name, value } } }
                for _h2, b2 in _iter_top_level_blocks(sub_body):
                    if not action:
                        action = _parse_kv_str(b2, "action_name")
                    for _h3, b3 in _iter_top_level_blocks(b2):
                        pname = _parse_kv_str(b3, "param_name")
                        if pname:
                            data.append((pname, _norm_scalar(_parse_kv_str(b3, "value"))))
        entities.append(Entity(phase, table, action, keys, data,
                               is_default=_parse_kv_bool(inner, "is_default_entry",
                                                         "is_default_action")))
    return entities


def _parse_traces(text: str) -> Dict[int, List[str]]:
    """Return {phase: [trace body, ...]} in emission order."""
    traces: Dict[int, List[str]] = {}
    for line in text.splitlines():
        m = _TRACE_RE.match(line)
        if m:
            traces.setdefault(int(m.group("phase")), []).append(m.group("body"))
    return traces


# ---------------------------------------------------------------------------
# Derived facts
# ---------------------------------------------------------------------------

def _extracted_fields(phase_traces: Sequence[str]) -> List[Tuple[str, str]]:
    """Ordered [(field, printed value)] pulled out of the input packet, in extract order.

    Only the first parser run counts. A second pipeline (Tofino egress, or a
    recirculation) re-parses the *deparsed* packet and re-extracts the same
    header instances from a fresh buffer, which would both duplicate names and
    break the one-variable-per-extracted-field numbering the caller calibrates
    against.
    """
    fields: List[Tuple[str, str]] = []
    seen_headers: Set[str] = set()
    parsers = 0
    for body in phase_traces:
        if body.startswith("[Parser]"):
            parsers += 1
            if parsers > 1:
                break
        if "[ExtractSuccess]" not in body:
            continue
        head, sep, rest = body.partition(" -> ")
        m = _EXTRACT_HEAD_RE.search(head)
        if not sep or m is None or m.group("hdr") in seen_headers:
            continue
        seen_headers.add(m.group("hdr"))
        for chunk in rest.split(" | "):
            name, eq, value = chunk.partition(" = ")
            if eq:
                fields.append((name.strip(), value.strip()))
    return fields


def _table_key_symbols(phase_traces: Sequence[str]) -> List[Tuple[str, List[str]]]:
    """Return [(table, [key operand text, ...])] for every table branch that hit."""
    out: List[Tuple[str, List[str]]] = []
    for body in phase_traces:
        if "Table Branch:" not in body or " | Key(s): " not in body:
            continue
        table = body.split("Table Branch:", 1)[1].split(" | Key(s): ", 1)[0].strip()
        keys_part = body.split(" | Key(s): ", 1)[1]
        for marker in ("| Chosen action:", "|Chosen action:", "| Arg(s):"):
            if marker in keys_part:
                keys_part = keys_part.rsplit(marker, 1)[0]
                break
        out.append((table, _split_top_level(keys_part)))
    return out


def _pktvar_field_map(phase_traces: Sequence[str], entities: Sequence[Entity],
                      phase: int) -> Dict[str, Tuple[str, str]]:
    """Map `pktvar_N` to the (header field, solved value) it was extracted from.

    Packet variables are minted in extraction order, but the numbering is
    offset by however many fields the target prepends (intrinsic metadata on
    Tofino, none on BMv2), and that offset is not printed anywhere. So it is
    *calibrated*: a table branch prints its key operands symbolically while the
    matching serialized entity names the same keys as header fields, which
    gives (pktvar, field) anchors. If every anchor agrees on one shift the map
    is trusted; otherwise the caller falls back to comparing raw operands.
    """
    ordered = _extracted_fields(phase_traces)
    if not ordered:
        return {}
    names = [f for f, _v in ordered]

    entity_keys: Dict[str, List[str]] = {}
    for e in entities:
        if e.phase == phase and e.table and e.table not in entity_keys:
            entity_keys[e.table] = [f for f, _v in e.keys]

    shifts: Set[int] = set()
    anchors = 0
    for table, operands in _table_key_symbols(phase_traces):
        fields = entity_keys.get(table)
        if not fields:
            # Tables are printed fully qualified in the trace but may be serialized short.
            for cand, cand_fields in entity_keys.items():
                if table.endswith("." + cand) or cand.endswith("." + table):
                    fields = cand_fields
                    break
        if not fields or len(fields) != len(operands):
            continue
        for operand, field in zip(operands, fields):
            m = _PKTVAR_RE.fullmatch(operand.strip())
            if not m or names.count(field) != 1:
                continue   # not a packet variable, or an ambiguous anchor
            anchors += 1
            shifts.add(names.index(field) - int(m.group("name").split("_")[1]))

    if anchors == 0 or len(shifts) != 1:
        return {}
    shift = shifts.pop()
    return {f"pktvar_{i - shift}": entry
            for i, entry in enumerate(ordered) if i - shift >= 0}


class IndexExpr:
    """One register-index expression as printed in a trace."""

    def __init__(self, text: str):
        self.text = " ".join(text.split())

    @property
    def is_hash(self) -> bool:
        return bool(_CONCOLIC_HASH_RE.search(self.text))

    @property
    def hash_label(self) -> str:
        m = _CONCOLIC_HASH_RE.search(self.text)
        return m.group(0) if m else ""

    @property
    def is_fully_concrete(self) -> bool:
        """True when the expression carries no symbolic operand, i.e. every input is a literal.

        A concolic hash over literal inputs HAS been resolved -- its value is whatever the CRC of
        those bytes is -- so a reported cell of 0 is a computed hash rather than the solver's
        default. Same condition `operands` uses for its fully-concrete case."""
        return not re.search(r"\|[^|(]+\([^)|]*\)\|", self.text)

    def operands(self, pktvar_fields: Dict[str, Tuple[str, str]],
                 cp_args: Optional[Dict[str, str]] = None) -> Tuple[Tuple[str, str], ...]:
        """The index's symbolic operands as (label, solved value) pairs.

        The label is the header field when it could be recovered and the raw
        symbol otherwise. Values come from the parser trace for packet
        variables and from the action bodies for control-plane action
        arguments, which is what lets an index built from table action data be
        compared across phases at all; an operand with neither resolves to "".
        """
        syms = [m.group("name") for m in re.finditer(r"\|(?P<name>[^|(]+)\([^)|]*\)\|", self.text)]
        if not syms:
            # Fully concrete index: the expression text itself is the identity.
            return ((self.text, self.text),)
        resolved = []
        for s in syms:
            if s in pktvar_fields:
                resolved.append(pktvar_fields[s])
            else:
                resolved.append((s, (cp_args or {}).get(s, "")))
        return tuple(resolved)

    @staticmethod
    def cell_key(ops: Tuple[Tuple[str, str], ...]) -> Optional[Tuple[str, ...]]:
        """The part of `operands` that decides the cell: the operand values.

        Two phases that hash *different* fields still hit the same cell when
        those fields hold the same value -- which is exactly what Phase-2 index
        steering produces -- so the cell is a function of the values, not of
        the field names. Values are normalised to plain integers so that a
        packet field, an action argument and an already-folded constant are
        comparable; that drops the operand width, which can only ever make the
        check accept a pair it should have split, never the reverse. Returns
        None when some operand has no solved value.
        """
        if any(not value or value == "Taint" for _label, value in ops):
            return None
        return tuple(_norm_scalar(value) for _label, value in ops)


def _register_indices(phase_traces: Sequence[str]) -> Tuple[Dict[str, List[IndexExpr]],
                                                            List[IndexExpr]]:
    """Return ({register name: [index expr]}, [index exprs with no register name]).

    Tofino names the register on every write-back; BMv2 names it on reads
    (`RegisterRead[name]`) but not on writes, hence the unnamed bucket.
    """
    named: Dict[str, List[IndexExpr]] = {}
    unnamed: List[IndexExpr] = []
    for body in phase_traces:
        for args in _balanced_call_args(body, "tofino_register_writeback"):
            if len(args) >= 3:
                named.setdefault(args[0].strip().strip('"'), []).append(IndexExpr(args[-1]))
        m = re.search(r"\[RegisterRead\[(?P<reg>[^\]]*)\]:\s*Index\s+(?P<rest>.*)$", body)
        if m:
            idx = m.group("rest").rsplit(" into field ", 1)[0]
            named.setdefault(m.group("reg").strip(), []).append(IndexExpr(idx))
        if "[RegisterWrite:" in body and " into index " in body:
            idx = body.split(" into index ", 1)[1].rstrip("]").strip()
            unnamed.append(IndexExpr(idx))
    return named, unnamed


def _cp_arg_bindings(phase_traces: Sequence[str]) -> Dict[str, str]:
    """Recover {action-arg symbol: concrete value} for one phase.

    The symbol itself never appears with its model value; what the trace does
    show is the action body assigning the formal parameter, with the solved
    value in the `Computed:` half. So bind each `..._arg_<param>` symbol from
    the assignment that copies `<param>` out, scanning only until the next
    table apply so a later action cannot donate a value to this one.
    """
    bindings: Dict[str, str] = {}
    for i, body in enumerate(phase_traces):
        if "[MethodCall]" not in body:
            continue
        syms = [(m.group("name"), m.group("name").rsplit("_arg_", 1)[1])
                for m in _CP_ARG_RE.finditer(body)]
        if not syms:
            continue
        # `switch_type_1` is the midend-uniquified spelling of parameter `switch_type`; the action
        # body prints the source name, so accept both.
        wanted: Dict[str, List[str]] = {}
        for name, param in syms:
            variants = [param]
            trimmed = re.sub(r"_\d+$", "", param)
            if trimmed != param:
                variants.append(trimmed)
            wanted[name] = variants
        for j in range(i + 1, min(i + 1 + 30, len(phase_traces))):
            nxt = phase_traces[j]
            if "Table Branch:" in nxt or ".apply();" in nxt:
                break
            m = _ASSIGN_RE.search(nxt)
            if m is None:
                continue
            rhs = m.group("rhs").strip()
            for name, variants in wanted.items():
                if rhs in variants:
                    bindings[name] = _norm_scalar(m.group("value"))
    return bindings


def _case_kind(path: Path, text: str) -> str:
    """The tamper kind tag (`h2m` / `m2h` / `adiv` / `c_t2f` / `c_f2t`), or "".

    "" means the file names no kind -- either it predates the metadata line and
    the file-name tag, or it was produced by something other than the tampering
    back ends. Callers must treat "" as "no claim either way" and keep whatever
    they did before, so an older corpus lints exactly as it used to.
    """
    m = _TAMPER_CASE_RE.search(text)
    if m is not None:
        tag = _CASE_KIND_TAGS.get(m.group("label"))
        if tag:
            return tag
    stem = path.stem
    for tag in _CASE_KIND_TAGS.values():
        if stem.endswith("_" + tag):
            return tag
    return ""


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def _check_sink_key_collision(path: Path, affected: List[dict],
                              entities: List[Entity], kind: str) -> List[str]:
    # An action-divergence case reaches the sink in BOTH replays and diverges by running a
    # DIFFERENT installed entry, so its attacker_value colliding with an installed key is the
    # mechanism, not a bug -- this check's "Phase-3 would still HIT" reasoning only applies to a
    # case that claims Phase 3 stops hitting. Kind "" (a pre-tag file) keeps the old behaviour.
    if kind == "adiv":
        return []

    table_keys: Dict[str, Set[str]] = {}
    for e in entities:
        for _f, v in e.keys:
            table_keys.setdefault(e.table, set()).add(v)

    errors: List[str] = []
    for a in affected:
        sink = a["sink_table"]
        if not sink:
            continue   # not a Key-sink chain; nothing to enforce
        if a["hit_phase"] < 0 or a["miss_phase"] < 0:
            # No hit/miss phase emitted, so the block states no HIT/MISS claim to violate (this
            # check's documented precondition). Independent of the kind tag above and true of every
            # action-divergence case, whatever its file is called: the generator writes the pair
            # only for the two HIT/MISS kinds. Every legacy file carrying `sink_table` carries it.
            continue
        for tname in [t.strip() for t in sink.split(",") if t.strip()]:
            if a["attacker_value"] in table_keys.get(tname, set()):
                errors.append(
                    f"{path}: [sink-key-collision] register={a['name']} index={a['index']} "
                    f"attacker_value collides with installed key in sink_table={tname} "
                    f"(Phase-3 would still HIT)")
    return errors


def _check_hash_index_default(path: Path, affected: List[dict],
                              per_phase_indices: Dict[int, Dict[str, List[IndexExpr]]],
                              reported_not_enforced: bool) -> List[str]:
    errors: List[str] = []
    for a in affected:
        exprs = [e for phase in sorted(per_phase_indices)
                 for e in per_phase_indices[phase].get(a["name"], [])]
        if not exprs:
            continue   # no index expression in the trace; nothing to judge
        hashed = [e for e in exprs if e.is_hash]
        if not hashed:
            continue   # directly-indexed register: cell 0 is an ordinary value
        if a["index"] != 0:
            continue
        # Index 0 on a hash-indexed register has two very different causes and only one is a defect:
        #
        #   unresolved  the concolic hash never got a value, so 0 is just the solver's default and
        #               the emitted cell is NOT the one the packet hashes to -- a broken case;
        #   degenerate  the hash WAS computed, over an all-zero input. CRC-16/ARC of all zeros is
        #               exactly 0, so the cell is right and the case would replay -- but it only
        #               "collides" because attacker and victim both carry an empty key, which no
        #               realistic victim does.
        #
        # The concolic call in the trace prints its operands, so the two are distinguishable: an
        # all-concrete-zero operand list means the hash resolved. Reporting them identically was
        # wrong and sent at least one investigation down the wrong path.
        note = " (case is marked index-reported-not-enforced)" if reported_not_enforced else ""
        if hashed[0].is_fully_concrete:
            errors.append(
                f"{path}: [hash-index-default] register={a['name']} is hash-indexed via "
                f"{hashed[0].hash_label} over a fully CONCRETE input{note}, so cell 0 is a computed "
                f"hash and not the solver default -- CRC-16 of an all-zero input really is 0. The "
                f"cell is correct; what is wrong is that the collision is degenerate, since "
                f"attacker and victim share it only because both keys are empty")
        else:
            errors.append(
                f"{path}: [hash-index-default] register={a['name']} reports index=0 but is "
                f"hash-indexed via {hashed[0].hash_label}{note}; 0 is the solver default for an "
                f"unresolved concolic hash, so the emitted cell is not the one the packet hashes to")
    return errors


def _describe_operands(ops: Tuple[Tuple[str, str], ...]) -> str:
    return "{" + ", ".join(f"{label}={value}" if value else label
                           for label, value in ops) + "}"


def _check_hash_input_phases(path: Path, affected: List[dict],
                             per_phase_indices: Dict[int, Dict[str, List[IndexExpr]]],
                             per_phase_unnamed: Dict[int, List[IndexExpr]],
                             pktvar_fields: Dict[int, Dict[str, Tuple[str, str]]],
                             cp_args: Dict[int, Dict[str, str]]) -> List[str]:
    errors: List[str] = []
    for a in affected:
        reg = a["name"]
        exprs_by_phase: Dict[int, List[IndexExpr]] = {}
        sides: Dict[int, List[Tuple[Tuple[str, str], ...]]] = {}
        for phase in (1, 2):
            exprs = per_phase_indices.get(phase, {}).get(reg, [])
            if not exprs:
                # BMv2 does not name the register on a write; fall back to the phase's writes
                # only when they are unambiguous.
                unnamed = per_phase_unnamed.get(phase, [])
                if len({u.text for u in unnamed}) == 1:
                    exprs = unnamed[:1]
            if exprs:
                exprs_by_phase[phase] = exprs
                sides[phase] = [e.operands(pktvar_fields.get(phase, {}), cp_args.get(phase, {}))
                                for e in exprs]
        if len(sides) < 2:
            continue   # one of the phases never touched this register in the trace
        if ({e.is_hash for e in exprs_by_phase[1]} != {e.is_hash for e in exprs_by_phase[2]}):
            # One phase indexes through a hash and the other with a folded constant. Deciding
            # whether they coincide would mean evaluating the hash, which the file does not
            # record, so make no claim.
            continue

        cells1 = {IndexExpr.cell_key(ops) for ops in sides[1]}
        cells2 = {IndexExpr.cell_key(ops) for ops in sides[2]}
        if None in cells1 or None in cells2:
            # At least one operand has no solved value (metadata the parser never extracted, or
            # taint), so fall back to comparing the operand labels and say the check is weaker.
            labels1 = {tuple(l for l, _v in ops) for ops in sides[1]}
            labels2 = {tuple(l for l, _v in ops) for ops in sides[2]}
            if labels1 & labels2:
                continue
            note = (" (some hash operands have no solved value; compared by operand identity, "
                    "not by cell)")
        else:
            if cells1 & cells2:
                continue   # a Phase-2 write hashes the same values, so it lands on the same cell
            note = ""
        p1 = sorted(_describe_operands(ops) for ops in sides[1])
        p2 = sorted(_describe_operands(ops) for ops in sides[2])
        errors.append(
            f"{path}: [hash-input-phase-mismatch] register={reg} Phase-1 hashes {', '.join(p1)} "
            f"but Phase-2 hashes {', '.join(p2)}; the attacker writes a different cell than the "
            f"victim reads{note}")
    return errors


def _check_cp_args(path: Path, entities: List[Entity],
                   bindings: Dict[int, Dict[str, str]]) -> List[str]:
    errors: List[str] = []

    # (a) serialized entries: the same table entry (same key set) must carry the same action and
    # the same action data in every phase that installs it.
    by_entry: Dict[Tuple[str, Tuple], List[Entity]] = {}
    for e in entities:
        if e.phase < 0 or not e.table:
            continue
        by_entry.setdefault((e.table, e.entry_signature), []).append(e)
    for (table, _sig), group in sorted(by_entry.items()):
        phases = {e.phase for e in group}
        if len(phases) < 2:
            continue
        actions = {e.action for e in group}
        if len(actions) > 1:
            errors.append(
                f"{path}: [cp-arg-phase-mismatch] table={table} same key installs different "
                f"actions across phases: " +
                ", ".join(f"P{e.phase}={e.action}" for e in sorted(group, key=lambda x: x.phase)))
            continue
        by_field: Dict[str, Dict[int, str]] = {}
        for e in group:
            for field, value in e.data:
                by_field.setdefault(field, {})[e.phase] = value
        for field, per_phase in sorted(by_field.items()):
            if len(set(per_phase.values())) > 1:
                errors.append(
                    f"{path}: [cp-arg-phase-mismatch] table={table} action data '{field}' "
                    "disagrees across phases: " +
                    ", ".join(f"P{p}={v}" for p, v in sorted(per_phase.items())))

    # (b) trace symbols: the view onto a default-action table whose entry was NOT serialized (output
    # generated before p4symbex emitted default entries). Tables that *are* serialized are left to
    # (a), which scopes the comparison to one entry -- the action-data symbol is named per
    # (table, action) with no key in it, so on a serialized table the two phases may simply have hit
    # different entries, and comparing the symbol across them would flag a legitimate difference.
    #
    # Note the coverage handover: once a default entry IS serialized, its table joins `serialized`
    # and the check moves from here to (a). That is only equivalent because the generator emits the
    # default entry for BOTH phases -- (a) needs two phases to compare. A one-phase emission would
    # silently drop the check on both sides.
    serialized = {e.table for e in entities if e.table}
    shared = set(bindings.get(1, {})) & set(bindings.get(2, {}))
    for sym in sorted(shared):
        if any(sym.startswith(table + "_") for table in serialized):
            continue
        v1, v2 = bindings[1][sym], bindings[2][sym]
        if v1 != v2:
            errors.append(
                f"{path}: [cp-arg-phase-mismatch] control-plane arg '{sym}' modelled as "
                f"P1={v1} but P2={v2}; the phases are separate solver queries, so the emitted "
                "test is not replayable as one control-plane configuration")
    return errors


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def validate_one(path: Path, enabled: Set[str], verbose: bool = False) -> List[str]:
    """Returns a list of human-readable error strings; empty on success."""
    text = path.read_text()

    entities = _parse_entities(text)
    traces = _parse_traces(text)
    affected: List[dict] = []
    metadata: List[str] = []
    for hdr, body in _iter_top_level_blocks(text):
        if hdr == "affected_register":
            affected.append({
                "name": _parse_kv_str(body, "register_name"),
                "index": _parse_kv_int(body, "index"),
                "attacker_value": _norm_scalar(_parse_kv_str(body, "attacker_value")),
                "sink_table": _parse_kv_str(body, "sink_table"),
                "hit_phase": _parse_kv_int(body, "hit_phase"),
                "miss_phase": _parse_kv_int(body, "miss_phase"),
            })
    for m in _KV_RE.finditer(text):
        if m.group(1) == "metadata":
            metadata.append(m.group(2))
    reported_not_enforced = any("index-reported-not-enforced" in m for m in metadata)
    kind = _case_kind(path, text)

    per_phase_indices: Dict[int, Dict[str, List[IndexExpr]]] = {}
    per_phase_unnamed: Dict[int, List[IndexExpr]] = {}
    pktvar_fields: Dict[int, Dict[str, Tuple[str, str]]] = {}
    extracted: Dict[int, List[Tuple[str, str]]] = {}
    bindings: Dict[int, Dict[str, str]] = {}
    for phase, phase_traces in traces.items():
        named, unnamed = _register_indices(phase_traces)
        per_phase_indices[phase] = named
        per_phase_unnamed[phase] = unnamed
        extracted[phase] = _extracted_fields(phase_traces)
        pktvar_fields[phase] = _pktvar_field_map(phase_traces, entities, phase)
        bindings[phase] = _cp_arg_bindings(phase_traces)

    # A phase whose calibration found no anchor (no table hit on a packet-derived key) can still
    # borrow another phase's numbering, but only when the two parsed the same field sequence:
    # cloning the init state re-pulls the same pktvar_N in order along the same parse path, and
    # nowhere else. Only the names have to match -- the solved values are read per phase.
    for phase, mapping in pktvar_fields.items():
        if mapping:
            continue
        names = [f for f, _v in extracted[phase]]
        for donor, donor_map in pktvar_fields.items():
            if donor == phase or not donor_map:
                continue
            if [f for f, _v in extracted[donor]] != names:
                continue
            values = dict(extracted[phase])
            pktvar_fields[phase] = {var: (field, values.get(field, ""))
                                    for var, (field, _v) in donor_map.items()}
            break

    if verbose:
        print(f"--- {path}  [kind: {kind or 'unknown'}]")
        for phase in sorted(traces):
            print(f"  P{phase}: {len(traces[phase])} trace events, "
                  f"{len(pktvar_fields[phase])} pktvars resolved")
            for reg, exprs in sorted(per_phase_indices[phase].items()):
                for e in exprs:
                    print(f"    index {reg}: "
                          f"{_describe_operands(e.operands(pktvar_fields[phase], bindings[phase]))}"
                          f"   [{e.hash_label or 'direct'}]")
            for e in per_phase_unnamed[phase]:
                print(f"    index <unnamed write>: "
                      f"{_describe_operands(e.operands(pktvar_fields[phase], bindings[phase]))}"
                      f"   [{e.hash_label or 'direct'}]")
            for sym, val in sorted(bindings[phase].items()):
                print(f"    cp-arg {sym} = {val}")
        for a in affected:
            print(f"  affected_register {a['name']} index={a['index']} "
                  f"attacker_value={a['attacker_value']} sink={a['sink_table']}")

    errors: List[str] = []
    if "sink-key-collision" in enabled:
        errors += _check_sink_key_collision(path, affected, entities, kind)
    if "hash-index-default" in enabled:
        errors += _check_hash_index_default(path, affected, per_phase_indices,
                                            reported_not_enforced)
    if "hash-input-phase-mismatch" in enabled:
        errors += _check_hash_input_phases(path, affected, per_phase_indices,
                                           per_phase_unnamed, pktvar_fields, bindings)
    if "cp-arg-phase-mismatch" in enabled:
        errors += _check_cp_args(path, entities, bindings)
    return errors


def main(argv: list[str]) -> int:
    enabled: Set[str] = set(CHECKS)
    only: Set[str] = set()
    verbose = False
    files: List[str] = []

    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg == "--verbose":
            verbose = True
        elif arg == "--list-checks":
            print("\n".join(CHECKS))
            return 0
        elif arg in ("--disable", "--only"):
            if i + 1 >= len(argv):
                print(f"{arg} needs a check name", file=sys.stderr)
                return 2
            name = argv[i + 1]
            if name not in CHECKS:
                print(f"unknown check '{name}' (known: {', '.join(CHECKS)})", file=sys.stderr)
                return 2
            if arg == "--only":
                only.add(name)
            else:
                enabled.discard(name)
            i += 1
        elif arg.startswith("--"):
            print(f"unknown option {arg}", file=sys.stderr)
            return 2
        else:
            files.append(arg)
        i += 1

    if only:
        enabled = only
    if not files:
        print("usage: lint_txtpb.py [--verbose] [--disable CHECK] [--only CHECK] "
              "<file1.txtpb> [<file2.txtpb> ...]", file=sys.stderr)
        return 2

    failures = 0
    checked = 0
    for f in files:
        p = Path(f)
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            failures += 1
            continue
        checked += 1
        for err in validate_one(p, enabled, verbose):
            print(err, file=sys.stderr)
            failures += 1
    if failures:
        print(f"lint_txtpb: {failures} failure(s) across {checked} file(s)", file=sys.stderr)
        return 1
    print(f"lint_txtpb: OK ({checked} file(s) clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
