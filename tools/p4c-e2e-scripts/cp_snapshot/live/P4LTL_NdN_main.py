"""NdN: P4LTL/NdN ships no control plane, so nothing runs; the capture confirms the loaded
pipeline alone (const entries, compiled defaults) yields an empty snapshot."""
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
DESCRIPTION = "no controller in the repo (topology only)"
PORTS = [Port(1, "HOST", "h1"), Port(2, "HOST", "h2"), Port(3, "ATTACKER", "attacker")]


def run(ctx):
    ctx.note("no control plane in P4LTL/NdN; nothing to run")
