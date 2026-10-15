"""P4xos acceptor: P4LTL/P4xos/control plane/config-acceptor.txt (simple_switch_CLI).

transport_tbl gets no entry (its default drops), as shipped: the setup gives the acceptor no
forwarding row. register_write sets the acceptor id, MAC and IP.
"""
from pathlib import Path

from cp_snapshot.cli2snap import convert
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
ROOT = Path("/root/jiwon/Workspace-remote/top_tier_repo/P4LTL/P4xos")
SOURCES = ["P4LTL/P4xos/control plane/config-acceptor.txt"]
NOTES = ["no transport_tbl forward row in the shipped config (the default drops every Paxos packet)"]
PORTS = [Port(1, "HOST", "h1"), Port(2, "HOST", "h2"), Port(3, "ATTACKER", "attacker")]


def build():
    return convert(ROOT / "acceptor.p4", [ROOT / "control plane/config-acceptor.txt"])
