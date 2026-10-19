"""P4-LRU LRUTable: testbed/LRUTable/P4/set_lru_table.py in bfshell."""
from cp_snapshot.capture import bfrt_name
from cp_snapshot.snapshot import Port

DESCRIPTION = "set_lru_table.py (bfrt_python, bfshell -b)"
SOURCE = "P4-LRU/testbed/LRUTable/P4"
PORTS = [Port(128, "HOST", "h128"), Port(144, "HOST", "h144"), Port(136, "ATTACKER", "attacker")]
PATCHES = [
    ("set_lru_table.py", "bfrt.lru_table.pipe1.",
     f"bfrt.{bfrt_name('P4-LRU_testbed_LRUTable_P4_lru_table')}.pipe1.",
     "program name: the testbed compiled it as lru_table"),
]


def run(ctx):
    out = ctx.bfshell(ctx.src / "set_lru_table.py")
    if "Error" in out or "Traceback" in out:
        raise RuntimeError("set_lru_table.py raised (see controller.log)")
