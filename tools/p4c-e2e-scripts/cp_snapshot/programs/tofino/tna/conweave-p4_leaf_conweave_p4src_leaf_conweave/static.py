"""conweave-p4_leaf_conweave_p4src_leaf_conweave: no control plane ships with the program, so nothing is installed after boot. Every
runtime table runs its default action and const entries apply as compiled (closed world).
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = []
NOTES = ["no control plane in the repo; snapshot = topology only (closed world: defaults)"]
PORTS = [Port(0, "HOST", "h0"), Port(1, "HOST", "h1"), Port(2, "ATTACKER", "attacker")]
# Not installed in the SDE (no bf-rt.json); nothing is installed, so there are no names to check.
SCHEMA_OPTIONAL = True


def build(r):
    pass
