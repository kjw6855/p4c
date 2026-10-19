"""Ribosome: setup.py in bfshell (README: run_bfshell.sh -i -b setup.py), up to the table setup.

setup.py ends by starting three 1 s threading.Timer loops (QP restore, overloaded servers, port
stats) that keep rewriting restore_qp and the qp_mapping selector at runtime; those belong to A2S
and are not started. access.txt is run as shipped (its commands were removed by the authors: NDA).
"""
from cp_snapshot.capture import bfrt_name
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
