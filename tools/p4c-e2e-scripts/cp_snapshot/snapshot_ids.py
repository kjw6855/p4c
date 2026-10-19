#!/usr/bin/env python3
"""Rewrite the bmv2 snapshot_entities blocks of tampering cases from names to P4Info ids.

    snapshot_ids.py <p4info.json> <case.txtpb> ...      (rewrites the files in place)

p4symbex names snapshot table entries through p4c's p4runtime fork (table_name, field_name,
action_name, param_name). The fork renumbers TableEntry/FieldMatch fields, so a message built with
it is not wire-compatible with an upstream P4Runtime server such as simple_switch_grpc. Rewriting the
names to ids makes each block valid upstream text format; tampering.install_snapshot then sends it
unchanged.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def _index(p4info: dict):
    actions = {a["preamble"]["name"]: a for a in p4info.get("actions", [])}
    tables = {t["preamble"]["name"]: t for t in p4info.get("tables", [])}
    return tables, actions


def rewrite(text: str, tables: dict, actions: dict) -> str:
    out, table, action = [], None, None
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("#"):
            out.append(line)
            continue
        m = re.match(r'(\s*)table_name: "([^"]+)"', line)
        if m:
            table = tables[m.group(2)]
            line = f"{m.group(1)}table_id: {table['preamble']['id']}\n"
        m = re.search(r'field_name: "([^"]+)"', line)
        if m and table is not None:
            fid = next(f["id"] for f in table["matchFields"] if f["name"] == m.group(1))
            line = line.replace(m.group(0), f"field_id: {fid}")
        m = re.search(r'action_name: "([^"]+)"', line)
        if m:
            action = actions[m.group(1)]
            line = line.replace(m.group(0), f"action_id: {action['preamble']['id']}")
        for pm in re.finditer(r'param_name: "([^"]+)"', line):
            pid = next(p["id"] for p in action.get("params", []) if p["name"] == pm.group(1))
            line = line.replace(pm.group(0), f"param_id: {pid}")
        out.append(line)
    return "".join(out)


def main() -> int:
    tables, actions = _index(json.loads(Path(sys.argv[1]).read_text()))
    for f in sys.argv[2:]:
        p = Path(f)
        p.write_text(rewrite(p.read_text(), tables, actions))
    return 0


if __name__ == "__main__":
    sys.exit(main())
