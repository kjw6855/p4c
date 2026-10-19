"""NdN: no control-plane file ships with P4LTL/NdN. count_table's rules are const entries in the
P4, so the snapshot holds only the topology; every other table runs its default action. The
CPI_SIMP lines of NdN/6.p4ltl and 7.p4ltl constrain rules for verification; they are not rules.
"""
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
SOURCES = []
NOTES = ["no control plane in the repo; snapshot = topology only (closed world: defaults)"]
PORTS = [Port(1, "HOST", "h1"), Port(2, "HOST", "h2"), Port(3, "ATTACKER", "attacker")]


def build():
    return []
