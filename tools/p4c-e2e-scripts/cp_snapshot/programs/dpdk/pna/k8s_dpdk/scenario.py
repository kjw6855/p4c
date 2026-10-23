"""k8s_dpdk (k8s_dp.p4, PNA on DPDK/infrap4d): rules are installed and updated by hand.

No controller runs here: the operator starts infrap4d, loads the pipeline and installs the rules,
then captures over P4Runtime with capture.py --manual:

    capture.py k8s_dpdk --manual init [--sequence NAME]        # (1) the rules installed now
    ... update rules by hand (p4rt-ctl, a P4Runtime client) ...
    capture.py k8s_dpdk --manual step [--sequence NAME] [--trigger TEXT]   # (2) the diff
    (repeat (2) for 3-2, 4-3, ...)
    capture.py k8s_dpdk --manual clear [--sequence NAME]       # delete the sequence, start over

init on an existing sequence asks once whether to delete it and quits if not (--yes: delete).

The sequence goes to snapshot/seq/<NAME>.txtpb (NAME defaults to "manual"). P4RT_ADDR, P4INFO,
P4RT_DEVICE_ID, P4RT_CERTS and P4RT_SERVER_NAME in the environment override the values below.
They follow infrap4d's defaults, as p4rt-ctl uses them: TLS with the stratum certificates (issued
for localhost) and device id 1.
"""
from pathlib import Path

PROGRAM = "k8s_dpdk_k8s_dp"  # the chain cache's name for k8s_dp.p4
TARGET = "dpdk"
DESCRIPTION = "k8s_dp.p4 rules installed and updated by hand over P4Runtime (infrap4d)"
P4RT_ADDR = "localhost:9559"  # the name infrap4d's server certificate is issued for
P4RT_CERTS = "/usr/share/stratum/certs"  # ca.crt, client.crt, client.key (P4RT_CERTS= : plaintext)
P4RT_DEVICE_ID = 1
P4INFO = str(Path(__file__).with_name("p4Info.txt"))  # top_tier_repo/k8s_dpdk/p4Info.txt
PORTS = []  # roles for p4symbex; capture itself sends no packets
