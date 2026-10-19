"""SwitchV2P: ptf-tests/test.py setup_env(bfrt_info, 0, SwitchType.TOR), the setup every TOR test
runs first (BfRt gRPC through py/*.py). Only the setup runs: each PTF test sends packets right
after it, which would fill the cache registers.

A small harness binds a BfRt gRPC client and calls setup_env directly instead of going through
run_p4_tests.sh, whose port map would also shuffle swports at random.
"""
from cp_snapshot.capture import PTF_PRELUDE
from cp_snapshot.snapshot import Port

DESCRIPTION = "ptf-tests/test.py setup_env(bfrt_info, 0, TOR) via a BfRt gRPC harness"
SOURCE = "SwitchV2P/p4-prototype"
# swports[0], [1] face hosts (132.68.0.2 and the gateway); swports[2] is the mirror port.
PORTS = [Port(0, "HOST", "h0", ip="132.68.0.2"), Port(1, "HOST", "gw", ip="132.68.0.3"),
         Port(2, "LINK", "mirror"), Port(3, "ATTACKER", "attacker")]
PATCHES = [
    ("ptf-tests/test.py", "swports = get_sw_ports()", "swports = [0, 1, 2]",
     "PTF port map: get_sw_ports() shuffles the ports at random; fix the sorted default map"),
]

HARNESS = '''
import sys
import bfrt_grpc.client as gc
sys.path[:0] = ["py", "ptf-tests"]
import test
from switch_config import SwitchType
iface = gc.ClientInterface("127.0.0.1:50052", client_id=1, device_id=0, num_tries=30)
iface.bind_pipeline_config("{program}")
test.setup_env(iface.bfrt_info_get("{program}"), 0, SwitchType.TOR)
iface.tear_down_stream()
print("setup_env done")
'''


def run(ctx):
    (ctx.src / "capture_harness.py").write_text(PTF_PRELUDE + HARNESS.format(program=ctx.program))
    rc = ctx.python(["capture_harness.py"], ctx.src,
                    {"PYTHONPATH": ctx.env["PYTHONPATH"] + ":" + ctx.site + "/p4testutils"})
    if rc != 0:
        raise RuntimeError(f"setup_env harness rc={rc}")
