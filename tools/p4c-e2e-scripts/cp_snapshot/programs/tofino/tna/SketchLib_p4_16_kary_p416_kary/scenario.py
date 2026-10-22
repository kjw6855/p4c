"""SketchLib_p4_16_kary_p416_kary: no control plane ships with the program; validation boots a fresh switch and runs
nothing (static.py writes the matching empty snapshot).
"""
from cp_snapshot.snapshot import Port

DESCRIPTION = "no control plane (fresh switch)"
PORTS = [Port(0, "HOST", "h0"), Port(1, "HOST", "h1"), Port(2, "ATTACKER", "attacker")]


def run(ctx):
    pass
