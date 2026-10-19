"""cheetah stateful: s1-commands-stateful.txt fed to simple_switch_CLI, as p4run's cli_input does
for s1 in p4app-stateful.json (h1..h3 on s1 ports 1..3)."""
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
DESCRIPTION = "simple_switch_CLI < s1-commands-stateful.txt (p4app-stateful.json cli_input)"
SOURCE = "cheetah-p4"
PORTS = [Port(1, "HOST", "h1", ip="10.0.0.1"), Port(2, "HOST", "h2", ip="10.0.0.2"),
         Port(3, "HOST", "h3", ip="10.0.0.3"), Port(4, "ATTACKER", "attacker")]


def run(ctx):
    ctx.cli((ctx.src / "s1-commands-stateful.txt").read_text())
