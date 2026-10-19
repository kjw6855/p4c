"""Cebinae: tofino_prototype/main.py cebinae (libs/mgr.py: BfRt gRPC + fixed-function PD thrift),
as p4tapp.sh runs it once the switch is up.

The authors' switchd is a custom C++ build (main.cpp) whose cebinae_loop configures TM queues and
pktgen, then rewrites tiGetRates/tiCalcSaturated/tiReadLastSize every interval: queue/pktgen setup
is not table state and the loop is runtime (A2S). Stock bf_switchd serves the same BfRt and PD
fixed-function thrift (pal/conn_mgr/mc on :9090), so main.py runs unchanged against it.
"""
from cp_snapshot.capture import PTF_PRELUDE
from cp_snapshot.snapshot import Port

DESCRIPTION = "main.py cebinae (BfRt gRPC + PD fixed-function thrift); C++ rate loop not run"
SOURCE = "Cebinae/tofino_prototype"
# commonStart: plum 144, peacock 155, green 153, mustard 163; bottleneck sides 128 and 136.
PORTS = [Port(144, "HOST", "plum"), Port(155, "HOST", "peacock"), Port(153, "HOST", "green"),
         Port(163, "HOST", "mustard"), Port(128, "LINK", "bottleneck1"),
         Port(136, "LINK", "bottleneck2"), Port(140, "ATTACKER", "attacker")]
PATCHES = [
    ("main.py", 'm = Manager("main")', 'm = Manager("Cebinae_tofino_prototype_main")',
     "program name: the authors compiled it as main"),
]


HARNESS = """
import runpy, sys
sys.argv = ["main.py", "cebinae"]
runpy.run_path("main.py", run_name="__main__")
"""


def run(ctx):
    # mgr.py imports p4testutils, which reads PTF's test params at import (p4tapp.sh runs it
    # inside a PTF environment); the harness provides them and runs main.py unchanged.
    (ctx.src / "capture_harness.py").write_text(PTF_PRELUDE + HARNESS)
    rc = ctx.python(["capture_harness.py"], ctx.src,
                    {"PYTHONPATH": ctx.env["PYTHONPATH"] + ":" + ctx.site + "/p4testutils"})
    if rc != 0:
        raise RuntimeError(f"main.py rc={rc}")
