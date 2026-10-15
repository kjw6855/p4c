"""cheetah stateful: cheetah-p4/s1-commands-stateful.txt (simple_switch_CLI), topology
p4app-stateful.json (h1..h3 on s1 ports 1..3).

Lines for get_server_from_ip / server_to_transition_state / server_timestamps (not in
stateful.p4) and the `register_Write` typo lines are rejected by the switch and skipped.
"""
from pathlib import Path

from cp_snapshot.cli2snap import convert
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
ROOT = Path("/root/jiwon/Workspace-remote/top_tier_repo/cheetah-p4")
SOURCES = ["cheetah-p4/s1-commands-stateful.txt", "cheetah-p4/p4app-stateful.json"]
PORTS = [Port(1, "HOST", "h1", ip="10.0.0.1"), Port(2, "HOST", "h2", ip="10.0.0.2"),
         Port(3, "HOST", "h3", ip="10.0.0.3"), Port(4, "ATTACKER", "attacker")]


def build():
    return convert(ROOT / "cheetah-stateful.p4", [ROOT / "s1-commands-stateful.txt"])
