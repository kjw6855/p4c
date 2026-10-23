"""Ribosome: setup.py in bfshell (README: run_bfshell.sh -i -b setup.py), up to the table setup.

setup.py ends by starting three 1 s threading.Timer loops (QP restore, overloaded servers, port
stats) that keep rewriting restore_qp and the qp_mapping selector at runtime; those belong to A2S
and are not started (one restore_qp tick runs as a snapshot-sequence STEP). access.txt is run as
shipped (its commands were removed by the authors: NDA).
"""
from cp_snapshot.capture import bfrt_name
from cp_snapshot.packages import ControllerStep
from cp_snapshot.snapshot import Port

DESCRIPTION = "setup.py (bfrt_python, bfshell -b) up to the table setup; runtime timers not started"
SOURCE = "Ribosome-P4"
PORTS = ([Port(p, "HOST", f"output{i + 1}") for i, p in enumerate((48, 52, 44, 40))]
         + [Port(36, "LINK", "nf")]
         + [Port(p, "LINK", f"server{i + 1}") for i, p in enumerate((0, 20, 16, 32))]
         + [Port(8, "ATTACKER", "attacker")])
PATCHES = [
    ("setup.py", "p4 = bfrt.ribosome.pipe", f"p4 = bfrt.{bfrt_name('Ribosome-P4_ribosome')}.pipe",
     "program name: the authors compiled it as ribosome"),
    ("setup.py", 'curr_path = os.path.join(os.environ[\'HOME\'], "labs", "Ribosome-P4")',
     'curr_path = "{SRC}"', "path: the authors' checkout location"),
    ("setup.py", "restore_qp_timer()\noverloaded_servers_timer()\nport_stats_timer()",
     "pass  # runtime timers not started (capture point: after setup)",
     "stop before the runtime loops (they rewrite restore_qp and the selector every second)"),
]


def run(ctx):
    out = ctx.bfshell(ctx.src / "setup.py")
    if "Traceback" in out:
        raise RuntimeError("setup.py raised (see controller.log)")


# Runtime step: one tick of restore_qp_timer(), i.e. setup.py's set_restore_qp(): restore
# (restore_qp[i] = 1) every QP i the data plane disabled (enabled_qp[i] == 0) that has a queue pair
# (qp[i] > 0). Re-implemented over BfRt gRPC, the same reads and writes: a second bfrt_python
# session crashes bf_switchd (segfault) once setup.py's has run. One difference: setup.py writes
# restore_qp in asymmetric mode to pipe 0 only; here every pipe gets the write.
# The data-plane state it acts on comes from an rdma_qp_info packet (qp) and an AETH NAK
# (enabled_qp); the step stands in for those packets with register writes, which stay out of the
# step's updates (only restore_qp is declared as written by the control plane).
QP_INDEX = 5
QP_NUM = 0x123


def _cell(data: dict, field: str) -> int:
    v = data[field]
    return int(v[0] if isinstance(v, list) else v)


def _restore_qp(ctx):
    from tofino import tofino_driver as td
    from tofino.bfrt_grpc_client import BfRtClient
    c = BfRtClient(td.BFRT_GRPC_ADDR, client_id=1, p4_name=ctx.program, enable_learn=False)
    try:
        c.write_register("pipe.enabled_qp", QP_INDEX, [("enabled_qp.f1", 0)])  # an AETH NAK
        c.write_register("pipe.qp", QP_INDEX, [("qp.f1", QP_NUM)])  # an rdma_qp_info packet
        _, enabled = c.read_register_all("pipe.enabled_qp")
        _, qps = c.read_register_all("pipe.qp")
        disabled = [i for i, d in enabled.items() if _cell(d, "enabled_qp.f1") == 0]
        restored = [i for i in disabled if _cell(qps[i], "qp.f1") > 0]
        for i in restored:
            c.write_register("pipe.restore_qp", i, [("restore_qp.f1", 1)])
        ctx.note(f"set_restore_qp: {len(disabled)} disabled QPs, restored {restored}")
    finally:
        c.close()


STEPS = {"restore_qp": ControllerStep(
    _restore_qp, f"setup.py set_restore_qp() once (over BfRt gRPC), with QP {QP_INDEX} disabled "
    "by the data plane",
    registers=("restore_qp",))}
SCENARIOS = {"restore": ["restore_qp"]}
