#!/usr/bin/env python3
"""Validate a tampering .txtpb (BFRT or BMv2 protobuf flavour) against the
Key-sink HIT/MISS constraint.

For every affected_register block that carries `sink_table` / `hit_phase` /
`miss_phase` metadata, this script:

  1. Parses all entities blocks for the named sink table(s).
  2. Collects the set of installed match-key values (hex/escaped-bytes) for
     each named sink table.
  3. Asserts that the affected_register's `attacker_value` is NOT in that set
     — otherwise the Phase-3 lookup would still HIT, violating the user's
     "Phase 1 HIT, Phase 3 MISS" invariant.

Exit code 0 = all .txtpb files pass; non-zero = at least one collision
detected (a line per failing file printed to stderr).

Usage:  python3 validate_txtpb.py path/to/*.txtpb
"""

from __future__ import annotations

import codecs
import re
import sys
from pathlib import Path
from typing import Iterable, List, Tuple


_KV_RE = re.compile(r'(\w+)\s*:\s*"((?:[^"\\]|\\.)*)"')   # foo: "..."
_NUM_RE = re.compile(r'(\w+)\s*:\s*(-?\d+)')              # foo: 123


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


def validate_one(path: Path) -> List[str]:
    """Returns a list of human-readable error strings; empty on success."""
    text = path.read_text()
    # Collect installed keys per table: dict[table_name] = set of (field_name, value_bytes)
    table_keys: dict[str, set[Tuple[str, bytes]]] = {}
    affected: list[dict] = []

    for hdr, body in _iter_top_level_blocks(text):
        if hdr == "entities":
            tname = _parse_kv_str(body, "table_name")
            for sub_hdr, sub_body in _iter_top_level_blocks(body):
                if sub_hdr != "key":
                    continue
                fname = _parse_kv_str(sub_body, "field_name")
                for kind_hdr, kind_body in _iter_top_level_blocks(sub_body):
                    val = _parse_kv_str(kind_body, "value")
                    if val:
                        table_keys.setdefault(tname, set()).add((fname, _decode(val)))
        elif hdr == "affected_register":
            entry = {
                "name": _parse_kv_str(body, "register_name"),
                "index": _parse_kv_int(body, "index"),
                "attacker_value": _decode(_parse_kv_str(body, "attacker_value")),
                "sink_table": _parse_kv_str(body, "sink_table"),
                "hit_phase": _parse_kv_int(body, "hit_phase"),
                "miss_phase": _parse_kv_int(body, "miss_phase"),
            }
            affected.append(entry)

    errors: list[str] = []
    for a in affected:
        sink = a["sink_table"]
        if not sink:
            continue   # not a Key-sink chain; nothing to enforce
        for tname in [t.strip() for t in sink.split(",") if t.strip()]:
            keys = {v for (_f, v) in table_keys.get(tname, set())}
            if a["attacker_value"] in keys:
                errors.append(
                    f"{path}: register={a['name']} index={a['index']} "
                    f"attacker_value collides with installed key in sink_table={tname} "
                    f"(Phase-3 would still HIT)"
                )
    return errors


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: validate_txtpb.py <file1.txtpb> [<file2.txtpb> ...]", file=sys.stderr)
        return 2
    failures = 0
    checked = 0
    for f in argv[1:]:
        p = Path(f)
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            failures += 1
            continue
        checked += 1
        for err in validate_one(p):
            print(err, file=sys.stderr)
            failures += 1
    if failures:
        print(f"validate_txtpb: {failures} failure(s) across {checked} file(s)", file=sys.stderr)
        return 1
    print(f"validate_txtpb: OK ({checked} file(s) clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
