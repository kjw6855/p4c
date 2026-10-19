"""Mew edge coremelt: the PTF tests of edge/coremelt/initial_script/ (README: run_p4_tests.sh -p
mew_edge_coremelt -t .../initial_script/), i.e. mew_edge_coremelt.py (defense tables) and
mew_edge_coremelt_local.py (routing for the simplest edge--core topology, ports from
common/net_config.py).

A harness runs each test's runTest() over one BfRt gRPC client instead of the PTF runner (whose
BfRuntimeTest.setUp needs a PTF port map); the tests only add table entries. The reactor
(reactor_script/, a dp_poll + sniff loop) is runtime and not started.
"""
from cp_snapshot.capture import PTF_PRELUDE
from cp_snapshot.snapshot import Port

DESCRIPTION = "initial_script/ PTF tests (runTest) via a BfRt gRPC harness; reactor not started"
SOURCE = "Mew-prototype"
# net_config.py: host--port0--edge--port1--core; port2 is the second host-side port.
PORTS = [Port(0, "HOST", "host0"), Port(2, "HOST", "host2"), Port(1, "LINK", "to-core"),
         Port(10, "CPU", "cpu"), Port(3, "ATTACKER", "attacker")]
_NAME = "program name: the authors compiled it as mew_edge_coremelt"
PATCHES = [
    (f"edge/coremelt/initial_script/{f}", "self.p4_name='mew_edge_coremelt'",
     "self.p4_name='Mew-prototype_edge_coremelt_mew-edge-coremelt'", _NAME)
    for f in ("mew_edge_coremelt.py", "mew_edge_coremelt_local.py")
]

HARNESS = '''
import importlib, sys
sys.path[:0] = ["common", "edge/coremelt/initial_script"]
import bfrt_grpc.client as gc
import bfruntime_client_base_tests as base
iface = gc.ClientInterface("127.0.0.1:50052", client_id=1, device_id=0, num_tries=30)
iface.bind_pipeline_config("{program}")
def setUp(self, client_id, p4_name):  # PTF's BfRuntimeTest.setUp, minus the PTF port map
    self.interface = iface
base.BfRuntimeTest.setUp = setUp
for name in ("mew_edge_coremelt", "mew_edge_coremelt_local"):
    mod = importlib.import_module(name)
    t = mod.PortswitchTest()
    t.setUp()
    t.runTest()
    print(name, "runTest done")
iface.tear_down_stream()
'''


def run(ctx):
    (ctx.src / "capture_harness.py").write_text(PTF_PRELUDE + HARNESS.format(program=ctx.program))
    rc = ctx.python(["capture_harness.py"], ctx.src,
                    {"PYTHONPATH": ctx.env["PYTHONPATH"] + ":" + ctx.site + "/p4testutils"})
    if rc != 0:
        raise RuntimeError(f"initial_script harness rc={rc}")
