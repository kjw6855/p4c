"""SketchLib_p4_16_rhhh_p416_rhhh: no control plane ships with the program, so nothing is installed after boot. Every
runtime table runs its default action and const entries apply as compiled (closed world).
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = []
NOTES = ["no control plane in the repo; snapshot = topology only (closed world: defaults)"]
PORTS = [Port(0, "HOST", "h0"), Port(1, "HOST", "h1"), Port(2, "ATTACKER", "attacker")]


def build(r):
    pass
