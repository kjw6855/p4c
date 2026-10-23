#!/usr/bin/env python3
"""Offline checks of snapshot sequences (no target needed): updates() classifies INSERT, MODIFY and
DELETE (register cells and defaults only change), a sequence round-trips through its textproto,
materialize() applies steps in order, and every committed package snapshot re-parses to the same
textproto.

    python3 cp_snapshot/test_sequence_offline.py
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from cp_snapshot import snapshot as snap  # noqa: E402
from cp_snapshot.snapshot import Entity, Key, Port  # noqa: E402

FWD = "pipe.SwitchIngress.table_forward"
REG = "pipe.SwitchIngress.cache_conn"


def fwd(ip: int, port: int) -> Entity:
    return Entity(FWD, (Key("hdr.ipv4.dst_addr", "exact", ip),), "SwitchIngress.route",
                  (("dst_port", port),))


def reg(idx: int, val: int) -> Entity:
    return Entity(REG, (Key("$REGISTER_INDEX", "exact", idx),), "",
                  (("SwitchIngress.cache_conn.f1", val),), is_register=True)


def default(action: str) -> Entity:
    return Entity(FWD, (), action, (), is_default=True)


def check(cond: bool, what: str) -> None:
    if not cond:
        raise AssertionError(what)
    print(f"ok  {what}")


def test_updates() -> None:
    before = [fwd(1, 60), fwd(2, 61), reg(0, 0), reg(1, 5), default("SwitchIngress.drop")]
    after = [fwd(1, 62), fwd(3, 61), reg(0, 7), default("SwitchIngress.route")]
    ups = snap.updates(before, after)
    got = {(op, e.table, e.keys, e.data, e.action) for op, e in ups}
    check(("MODIFY", FWD, fwd(1, 62).keys, (("dst_port", 62),), "SwitchIngress.route") in got,
          "changed action data is a MODIFY")
    check(("INSERT", FWD, fwd(3, 61).keys, (("dst_port", 61),), "SwitchIngress.route") in got,
          "new entry is an INSERT")
    check(("DELETE", FWD, fwd(2, 61).keys, (("dst_port", 61),), "SwitchIngress.route") in got,
          "vanished entry is a DELETE")
    check(("MODIFY", REG, reg(0, 7).keys, reg(0, 7).data, "") in got, "register cell is a MODIFY")
    check(("MODIFY", REG, reg(1, 0).keys, reg(1, 0).data, "") in got,
          "vanished register cell reads as zeroed")
    check(any(op == "MODIFY" and e.is_default and e.action == "SwitchIngress.route"
              for op, e in ups), "default change is a MODIFY")
    check(len(ups) == 6, "no other updates")


def sample(target: str) -> snap.Sequence:
    init = snap.Snapshot(program="prog", target=target,
                         ports=[Port(60, "HOST", "h1", ip="10.0.0.1"), Port(63, "ATTACKER")],
                         entities=[fwd(1, 60), fwd(2, 61)], snapshot_id="init",
                         captured_at="2026-10-23T00:00:00+00:00")
    if target == "tofino":
        init.entities.append(reg(0, 0))
        regs = [("MODIFY", reg(0, 9))]
    else:
        init.entities.append(Entity("register_id:385", (Key("$index", "exact", 0),), "",
                                    (("value", 0),), is_register=True))
        regs = [("MODIFY", Entity("register_id:385", (Key("$index", "exact", 0),), "",
                                  (("value", 9),), is_register=True))]
    return snap.Sequence(init=init, scenario="demo", steps=[
        snap.Step("s1", "send digest packet", [("INSERT", fwd(3, 62))] + regs),
        snap.Step("s2", "failover", [("MODIFY", fwd(1, 63)), ("DELETE", fwd(2, 61))]),
    ])


def test_round_trip() -> None:
    for target in ("tofino", "bmv2"):
        seq = sample(target)
        text = snap.sequence_to_textproto(seq)
        back = snap.parse_sequence(text)
        check(snap.sequence_to_textproto(back) == text, f"{target}: write -> parse -> write")
        s2 = snap.materialize(back, 2)
        want = {fwd(1, 63), fwd(3, 62)}
        check(want <= set(s2.entities) and fwd(2, 61) not in s2.entities,
              f"{target}: materialize(2) applies both steps")
        s1 = snap.materialize(back, 1)
        check(fwd(2, 61) in s1.entities and fwd(1, 60) in s1.entities,
              f"{target}: materialize(1) stops after step 1")
    try:
        snap.apply_updates([fwd(1, 60)], [("DELETE", fwd(9, 0))])
        raise AssertionError("DELETE of an absent entry must fail")
    except KeyError:
        print("ok  DELETE of an absent entry fails")


def test_package_snapshots() -> None:
    """Every committed snapshot parses back to exactly its own textproto (comments aside)."""
    files = sorted((HERE / "programs").glob("*/*/*/snapshot/*.txtpb"))
    for f in files:
        text = f.read_text()
        body = "\n".join(line for line in text.splitlines() if not line.startswith("#")) + "\n"
        again = snap.to_textproto(snap.parse_snapshot(text))
        again = "\n".join(line for line in again.splitlines() if not line.startswith("#")) + "\n"
        check(again == body, f"re-parse {f.relative_to(HERE)}")


def test_a2s_sources() -> None:
    """a2s_sources: a step covers a chain through its cp sources or its stateful object."""
    from cp_snapshot import a2s_sources
    lab = "pipe.SwitchIngress.table_dst_tag_label"
    idx = "pipe.SwitchIngress.find_index"

    def label(ip: int, v: int, extra: int = 0) -> Entity:
        return Entity(lab, (Key("hdr.ipv4.dst_addr", "exact", ip),), "SwitchIngress.get_dst_label",
                      (("dst_label", v), ("other", extra)))

    def index(key: int, v: int) -> Entity:
        return Entity(idx, (Key("hdr.nc.key", "exact", key),), "SwitchIngress.find_index_act",
                      (("index", v),))
    chains = {"a2s2o": {"SwitchIngress": [
        {"id": 0, "soName": "SwitchIngress.cache_conn", "cp": {
            "writeData": [{"tables": ["SwitchIngress.table_dst_tag_label"],
                           "action": "SwitchIngress.get_dst_label", "param": "dst_label"}]}},
        {"id": 1, "soName": "SwitchIngress.seq_reg", "cp": {
            "writeIdx": [{"tables": ["SwitchIngress.find_index"],
                          "action": "SwitchIngress.find_index_act", "param": "index"}],
            "readIdx": [{"tables": ["SwitchIngress.find_index"],
                         "action": "SwitchIngress.find_index_act", "param": "index"}]}},
    ]}}
    init = snap.Snapshot(program="p", target="tofino", entities=[label(1, 5), index(7, 3)])

    def cover(*ups):
        seq = snap.Sequence(init=init, scenario="s", steps=[snap.Step("s1", "", list(ups))])
        return {(r["chain_id"], r["kind"]) for r in a2s_sources.coverage(seq, chains)}
    check(cover(("MODIFY", label(1, 6))) == {(0, "inconsistency")},
          "MODIFY of a writeData param -> inconsistency")
    check(cover(("MODIFY", label(1, 5, extra=9))) == set(),
          "MODIFY of another param -> not covered")
    check(cover(("INSERT", index(8, 4))) == {(1, "residual")},
          "INSERT into a writeIdx table -> residual")
    check(cover(("MODIFY", Entity("pipe.SwitchIngress.seq_reg",
                                  (Key("$REGISTER_INDEX", "exact", 3),), "", (("f1", 1),),
                                  is_register=True))) == {(1, "residual")},
          "register cell of the stateful object -> covered")
    check(cover(("INSERT", Entity("pipe.SwitchIngress.table_conn",
                                  (Key("hdr.ipv4.src_addr", "exact", 1),),
                                  "SwitchIngress.enforce_dec", (("decision", 0),)))) == set(),
          "an unrelated table -> not covered")


if __name__ == "__main__":
    test_updates()
    test_round_trip()
    test_a2s_sources()
    test_package_snapshots()
    print("all passed")
