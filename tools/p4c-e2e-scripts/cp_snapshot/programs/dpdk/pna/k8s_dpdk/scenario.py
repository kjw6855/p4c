"""k8s_dpdk (k8s_dp.p4, PNA on DPDK/infrap4d): rules are installed and updated by hand.

No controller runs here: the operator starts infrap4d, loads the pipeline and installs the rules,
then captures over P4Runtime with capture.py --manual:

    capture.py k8s_dpdk --manual init [--sequence NAME]        # (1) the rules installed now
    ... update rules by hand (p4rt-ctl, a P4Runtime client) ...
    capture.py k8s_dpdk --manual step [--sequence NAME] [--trigger TEXT]   # (2) the diff
    (repeat (2) for 3-2, 4-3, ...)

The sequence goes to snapshot/seq/<NAME>.txtpb (NAME defaults to "manual"). P4RT_ADDR, P4INFO and
P4RT_DEVICE_ID in the environment override the values below.
"""
from pathlib import Path

PROGRAM = "k8s_dpdk_k8s_dp"  # the chain cache's name for k8s_dp.p4
TARGET = "dpdk"
DESCRIPTION = "k8s_dp.p4 rules installed and updated by hand over P4Runtime (infrap4d)"
P4RT_ADDR = "127.0.0.1:9559"
P4INFO = str(Path(__file__).with_name("p4Info.txt"))  # top_tier_repo/k8s_dpdk/p4Info.txt
PORTS = []  # roles for p4symbex; capture itself sends no packets
