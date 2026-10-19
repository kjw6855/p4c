"""Brain-on-Switch normal version: testbed_version/setup/setup_all.py in bfshell (the README ships
control-plane code only for the testbed variant; its tables keep their names in the normal one).

setup_all.py runs setup_forward, setup_mirror, install_per_packet_entry, then setup_threshold,
which opens install/threshold.json. That file comes from the model-translation step and is not in
the repo, so the script stops there, as it would on a fresh checkout: the threshold tables (incl.
tab_global_collision_escalation_threshold) and install_table_entry's model tables stay empty.
"""
from cp_snapshot.capture import bfrt_name
from cp_snapshot.snapshot import Port

DESCRIPTION = "testbed_version/setup/setup_all.py (bfrt_python, bfshell -b)"
SOURCE = "Brain-on-Switch/p4/testbed_version"
_P = "bfrt." + bfrt_name("Brain-on-Switch_p4_normal_version_BoS_normal") + ".pipe."
PORTS = [Port(24, "HOST", "normal"), Port(28, "LINK", "escalation"),
         Port(68, "LINK", "mirror_pipe0"), Port(196, "LINK", "mirror_pipe1"),
         Port(0, "ATTACKER", "attacker")]
_NAME = "program name: testbed builds BoS_testbed with pipes pipe_a/pipe_b; normal has one pipe"
PATCHES = [
    ("setup/setup_all.py", "path_to_project = '/root/BoS/p4/testbed_version/'",
     "path_to_project = '{SRC}/'", "path: the authors' checkout location"),
] + [(f"setup/{f}", f"bfrt.BoS_testbed.{pipe}.", _P, _NAME)
     for f, pipes in (("setup_forward.py", ["pipe_a"]), ("setup_mirror.py", ["pipe_a"]),
                      ("install_per_packet_entry.py", ["pipe_b"]),
                      ("setup_threshold.py", ["pipe_a", "pipe_b"]),
                      ("install_table_entry.py", ["pipe_a", "pipe_b"]))
     for pipe in pipes]


def run(ctx):
    out = ctx.bfshell(ctx.src / "setup" / "setup_all.py")
    if "threshold.json" in out:
        ctx.note("setup_all.py stopped at setup_threshold: install/threshold.json is not in the "
                 "repo (model translation output)")
    elif "Traceback" in out or "Error" in out:
        raise RuntimeError("setup_all.py raised (see controller.log)")
