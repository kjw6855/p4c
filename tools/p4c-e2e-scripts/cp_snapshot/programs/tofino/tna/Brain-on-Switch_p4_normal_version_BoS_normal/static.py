"""Brain-on-Switch normal version: the testbed version's setup/setup_{forward,mirror,threshold}.py
(bfrt_python). The README ships control-plane code only for the testbed variant; its tables have the
same names in the normal version (BoS_testbed.pipe_a/pipe_b -> pipe).

setup_threshold.py reads install/threshold.json, which the model-translation step produces and the
repo does not ship: tab_confidence_threshold and tab_t_esc stay empty (closed world), as do the
model tables of install_table_entry.py. tab_global_collision_escalation_threshold needs only the
script's default ratio (0.05) and reset period (256).
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = ["Brain-on-Switch/p4/testbed_version/setup/setup_forward.py",
           "Brain-on-Switch/p4/testbed_version/setup/setup_mirror.py",
           "Brain-on-Switch/p4/testbed_version/setup/setup_threshold.py (collision part only)"]
NOTES = ["tab_execute_forward_action rules all have priority 0 in the script; kept in script order",
         "omitted (needs install/threshold.json, not in repo): tab_confidence_threshold, tab_t_esc"]
ESCALATION_PORT, NORMAL_PORT = 28, 24
PORTS = [Port(NORMAL_PORT, "HOST", "normal"), Port(ESCALATION_PORT, "LINK", "escalation"),
         Port(68, "LINK", "mirror_pipe0"), Port(196, "LINK", "mirror_pipe1"),
         Port(0, "ATTACKER", "attacker")]


def build(r):
    t = "SwitchIngress.tab_execute_forward_action"
    tern = lambda v, m: ("ternary", v, m)  # noqa: E731
    # collision-escalation
    r.entry(t, "forward", {"ig_use_rnn": tern(0, 0), "ig_use_per_packet": tern(0, 1),
                           "ig_collision": tern(1, 1), "ig_port": tern(0, 0),
                           "ig_stored_escalation_flag": tern(0, 0)}, {"port": ESCALATION_PORT})
    # rnn-escalation
    r.entry(t, "forward", {"ig_use_rnn": tern(1, 1), "ig_use_per_packet": tern(0, 0),
                           "ig_collision": tern(0, 0), "ig_port": tern(0, 0),
                           "ig_stored_escalation_flag": tern(0xff, 0xff)}, {"port": ESCALATION_PORT})
    # everything else
    r.entry(t, "forward", {"ig_use_rnn": tern(0, 0), "ig_use_per_packet": tern(0, 0),
                           "ig_collision": tern(0, 0), "ig_port": tern(0, 0),
                           "ig_stored_escalation_flag": tern(0, 0)}, {"port": NORMAL_PORT})
    # setup_mirror(num_pipe=2, sid_offset=4)
    for i in range(2):
        r.mirror_session(4 + i, 68 + i * 128, "EGRESS")
        r.entry("SwitchIngress.tab_set_egr_mir_ses", "set_egr_mir_ses", {"ig_pipe": i},
                {"egr_mir_ses": 4 + i})
    # setup_threshold: collision_escalation_reset_period=256, collision_escalation_ratio=0.05
    for i in range(256):
        r.entry("SwitchIngress.tab_global_collision_escalation_threshold",
                "act_global_collision_escalation_threshold", {"global_pkt_cnt": i},
                {"threshold": int(i * 0.05)})
