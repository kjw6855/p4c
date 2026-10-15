#!/usr/bin/env python3
"""Offline checks of the --cp-snapshot replay support in the Tofino harness (no SDE needed):
the parser reads snapshot_entities (with array/string data) and the Diverge output oracle, the
snapshot is installed fixed tables first, and the PORT oracle compares egress port sets only.

    python3 cp_snapshot/test_replay_offline.py
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tofino.packet_tester import PacketTester  # noqa: E402
from tofino.txtpb_parser import parse_tampering_txtpb  # noqa: E402

CASE = r'''
metadata: "Tamper case: OUTPUT_DIVERGE"
metadata: "Diverge output: PORT"
input_packet {
  packet: "\x00\x01"
  port: 1
}
input_packet {
  packet: "\x00\x02"
  port: 4
  tamper_only: true
}
input_packet {
  packet: "\x00\x01"
  port: 1
}
snapshot_entities {
  table_name: "pipe.Ingress.reg_p"
  key {
    field_name: "$REGISTER_INDEX"
    exact {
      value: "\x05"
    }
  }
  data {
    field_name: "Ingress.reg_p.f1"
    value: "\x00\x02"
  }
}
snapshot_entities {
  table_name: "$pre.mgid"
  key {
    field_name: "$MGID"
    exact {
      value: "\x01"
    }
  }
  data {
    field_name: "$MULTICAST_NODE_ID"
    int_arr_val: 16
  }
}
snapshot_entities {
  table_name: "$pre.node"
  key {
    field_name: "$MULTICAST_NODE_ID"
    exact {
      value: "\x10"
    }
  }
  data {
    field_name: "$DEV_PORT"
    int_arr_val: 1
    int_arr_val: 2
  }
}
snapshot_entities {
  table_name: "$mirror.cfg"
  action_name: "normal"
  key {
    field_name: "$sid"
    exact {
      value: "\x01"
    }
  }
  data {
    field_name: "$direction"
    str_val: "BOTH"
  }
  data {
    field_name: "$session_enable"
    bool_arr_val: true
  }
}
'''


class _RecordingClient:
    def __init__(self):
        self.calls = []

    def write_snapshot_entity(self, table, keys, datas, action, is_default):
        self.calls.append((table, [(d.field_name, d.value) for d in datas], action, is_default))


def main() -> int:
    fails = 0

    def check(name, cond, detail=""):
        nonlocal fails
        print(("PASS " if cond else "FAIL ") + name + (f" -- {detail}" if detail and not cond else ""))
        fails += not cond

    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "case.txtpb"
        p.write_text(CASE)
        case = parse_tampering_txtpb(p)
    check("diverge_output parsed", case.diverge_output == "PORT", case.diverge_output)
    check("4 snapshot entities", len(case.snapshot_entities) == 4, len(case.snapshot_entities))
    node = next(e for e in case.snapshot_entities if e.table_name == "$pre.node")
    check("int array data", node.data[0].value == [1, 2], node.data[0].value)
    mirror = next(e for e in case.snapshot_entities if e.table_name == "$mirror.cfg")
    check("string data", mirror.data[0].value == "BOTH", mirror.data[0].value)
    check("bool array data", mirror.data[1].value == [True], mirror.data[1].value)

    tester = PacketTester.__new__(PacketTester)
    tester.client = _RecordingClient()
    tester._install_snapshot(case.snapshot_entities)
    order = [c[0] for c in tester.client.calls]
    check("fixed tables first ($pre.node before $pre.mgid)",
          order[:3] == ["$pre.node", "$pre.mgid", "$mirror.cfg"], order)

    ok, _ = tester.compare_runs({2: [b"a"]}, {3: [b"a"]}, "PORT")
    check("PORT: port change is a finding", ok)
    ok, _ = tester.compare_runs({2: [b"a"]}, {2: [b"b"]}, "PORT")
    check("PORT: byte-only change is not", not ok)
    ok, _ = tester.compare_runs({1: [b"a"], 2: [b"a"]}, {3: [b"a"]}, "PORT")
    check("PORT: multicast port set {1,2} -> {3} is a finding", ok)
    ok, _ = tester.compare_runs({2: [b"a"]}, {}, "PORT")
    check("PORT: forward -> drop is a finding", ok)
    ok, _ = tester.compare_runs({2: [b"a"]}, {2: [b"b"]}, "")
    check("default oracle: byte change is still a finding", ok)
    print("ALL PASS" if fails == 0 else f"{fails} FAILURE(S)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
