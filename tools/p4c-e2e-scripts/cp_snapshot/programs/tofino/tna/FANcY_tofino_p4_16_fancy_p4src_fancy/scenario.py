"""FANcY: tofino/p4_16/fancy/control_plane/control_plane.py, FancyController().configure_all() in
model mode (BfRt gRPC through bfrt_helper/bfrt_grpc_helper.py; the program name is auto-detected).

The script reads "model" from sys.argv at import time to pick the model ports PORT*_M = 0..6, but its
argparse then rejects that argument; a harness imports it with that argv and calls configure_all()
directly, as its __main__ would. ipdb and tabulate (debug printing only) are stubbed.
"""
from cp_snapshot.snapshot import Port

DESCRIPTION = "control_plane.py FancyController().configure_all() (model mode, BfRt gRPC)"
SOURCE = "FANcY/tofino"  # control_plane.py imports ../../../eval
PORTS = [Port(p, "HOST", f"p{p}") for p in range(7)] + [Port(7, "ATTACKER", "attacker")]

HARNESS = r'''
import sys, types
sys.modules["ipdb"] = types.ModuleType("ipdb")
tab = types.ModuleType("tabulate"); tab.tabulate = lambda *a, **k: ""; sys.modules["tabulate"] = tab
sys.argv = ["control_plane.py", "model"]
sys.path.insert(0, ".")
import control_plane as cp
cp.FancyController().configure_all()
print("configure_all done")
'''


def run(ctx):
    cp_dir = ctx.src / "p4_16" / "fancy" / "control_plane"
    (cp_dir / "capture_harness.py").write_text(HARNESS)
    ctx.note("argparse bug: 'model' is read from sys.argv but rejected; harness calls "
             "configure_all() directly")
    rc = ctx.python(["capture_harness.py"], cp_dir)
    if rc != 0:
        raise RuntimeError(f"configure_all harness rc={rc}")
