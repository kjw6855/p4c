"""P4xos learner: `control plane/config-learner.txt` fed to simple_switch_CLI (Makefile rule)."""
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
DESCRIPTION = "simple_switch_CLI < control plane/config-learner.txt"
SOURCE = "P4LTL/P4xos/control plane"
PORTS = [Port(1, "HOST", "h1"), Port(2, "HOST", "h2"), Port(3, "ATTACKER", "attacker")]


def run(ctx):
    ctx.cli((ctx.src / "config-learner.txt").read_text())
