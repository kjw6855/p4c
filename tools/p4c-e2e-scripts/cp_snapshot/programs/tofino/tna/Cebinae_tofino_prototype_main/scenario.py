"""Cebinae: tofino_prototype/main.py cebinae (libs/mgr.py: BfRt gRPC + fixed-function PD thrift),
as p4tapp.sh runs it once the switch is up.

The authors' switchd is a custom C++ build (main.cpp) whose cebinae_loop configures TM queues and
pktgen, then rewrites tiGetRates/tiCalcSaturated/tiReadLastSize every interval: queue/pktgen setup
is not table state and the loop is runtime (A2S; one interval runs as snapshot-sequence STEPS).
Stock bf_switchd serves the same BfRt and PD fixed-function thrift (pal/conn_mgr/mc on :9090), so
main.py runs unchanged against it.
"""
from cp_snapshot.capture import PTF_PRELUDE
from cp_snapshot.packages import ControllerStep
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


# Runtime steps: one cebinae_loop() interval (main.cpp:777-923) in which the bottleneck port is
# saturated by one heavy-hitter flow, re-implemented over BfRt gRPC. The loop is C++ against the PD
# API, which the stock switchd does not serve, and its trigger (a pktgen rotate packet flipping
# ri_headq, then >= 90% of 25 Gb in the interval) cannot happen on tofino-model. The writes are the
# loop's: set_saturated -> tiCalcSaturated, set_bottlenecked -> tiCalcF, set_top/bottom_rate ->
# tiGetRates_head/_tail[f][headq], set_last_size -> tiReadLastSize. Values are the ones the loop
# computes for the flow below (main.cpp passes the 32-bit rates through htons, a bug; the intended
# values are written). The interval's writes are split into two steps so the scenarios can order
# them both ways.
BOTTLENECK_PORT = 128
TEST_PIPE = 1
PORT_BYTES_PER_DT = int(25e9 / 8 * 1.0)  # PORT_RATE_GBPS 25, DT_MS 1000
TAX_RATE = 0.05
HEAVY_SRC, HEAVY_DST = 0x0A000001, 0x0A000002  # 10.0.0.1 -> 10.0.0.2
HEAVY_BYTES = 2_800_000_000  # >= 90% of PORT_BYTES_PER_DT: the port is saturated
T = "pipe.Ingress.t_igr."


def _client(ctx):
    from tofino import tofino_driver as td
    from tofino.bfrt_grpc_client import BfRtClient
    return BfRtClient(td.BFRT_GRPC_ADDR, client_id=1, p4_name=ctx.program, enable_learn=False)


def _add(c, table, keys, action, data=()):
    from tofino.txtpb_parser import DataField, KeyField
    c.insert_entity(T + table, [KeyField(k, "exact", v) for k, v in keys],
                    [DataField(n, v) for n, v in data], "Ingress.t_igr." + action)


def _saturate(ctx):
    """set_saturated(128) and set_bottlenecked(heavy hitter)."""
    c = _client(ctx)
    try:
        _add(c, "tiCalcSaturated", [("ig_tm_md.ucast_egress_port", BOTTLENECK_PORT)], "aiSaturated")
        _add(c, "tiCalcF", [("hdr.ip.src_addr", HEAVY_SRC), ("hdr.ip.dst_addr", HEAVY_DST)],
             "aiSetFTop")
    finally:
        c.close()


def _rates(ctx):
    """set_top_rate / set_bottom_rate at the current headq, then set_last_size (first interval:
    the last sizes are 0)."""
    c = _client(ctx)
    try:
        cell = c.read_register(T + "ri_headq", 0) or {}
        v = next(iter(cell.values()), 0)
        headq = int(v[TEST_PIPE] if isinstance(v, list) and len(v) > TEST_PIPE
                    else (v[0] if isinstance(v, list) else v))
        top = int(HEAVY_BYTES * (1.0 - TAX_RATE))
        bottom = PORT_BYTES_PER_DT - top
        for f, n_bytes in ((BOTTLENECK_PORT << 1, top), (BOTTLENECK_PORT, bottom)):
            r = int(n_bytes / (1000 * 1000.0))  # bytes per us
            _add(c, "tiGetRates_head", [("f", f), ("headq", headq)], "aiGetRates_head",
                 [("r_head", r), ("r_head_x_dT", n_bytes)])
            _add(c, "tiGetRates_tail", [("f", f), ("tailq", headq)], "aiGetRates_tail",
                 [("r_tail", r), ("r_tail_x_dT", n_bytes)])
        for f in (BOTTLENECK_PORT << 1, BOTTLENECK_PORT):
            _add(c, "tiReadLastSize", [("f", f)], "aiReadLastSize", [("lastRate_x_dT", 0)])
        ctx.note(f"cebinae interval at headq={headq}: top {top} B, bottom {bottom} B")
    finally:
        c.close()


STEPS = {
    "saturate": ControllerStep(_saturate, "cebinae_loop: port 128 saturated, 10.0.0.1->10.0.0.2 "
                                          "is the heavy hitter (tiCalcSaturated, tiCalcF)"),
    "rates": ControllerStep(_rates, "cebinae_loop: top/bottom rates at the current headq "
                                    "(tiGetRates_head/_tail, tiReadLastSize)"),
}
SCENARIOS = {"saturate_rates": ["saturate", "rates"], "rates_saturate": ["rates", "saturate"]}
