"""P4xos learner: P4LTL/P4xos/control plane/config-learner.txt (simple_switch_CLI).

`table_add forward_tbl ...` names a table learner.p4 does not have and is rejected; no
transport_tbl row forwards (set_drop=1 -> _drop only), as shipped.
"""
from pathlib import Path

from cp_snapshot.cli2snap import convert
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
ROOT = Path("/root/jiwon/Workspace-remote/top_tier_repo/P4LTL/P4xos")
SOURCES = ["P4LTL/P4xos/control plane/config-learner.txt"]
PORTS = [Port(1, "HOST", "h1"), Port(2, "HOST", "h2"), Port(3, "ATTACKER", "attacker")]


def build():
    return convert(ROOT / "learner.p4", [ROOT / "control plane/config-learner.txt"])
