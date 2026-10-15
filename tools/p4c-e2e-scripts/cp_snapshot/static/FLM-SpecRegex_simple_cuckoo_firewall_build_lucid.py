"""FLM-SpecRegex simple_cuckoo_firewall: simple_cuckoo_firewall_build/lucid.py (Lucid controldriver).

Every table of the compiled program has const entries, so the controller installs no table rule:
it brings up ports 128-131 and adds the multicast groups. 640-643 flood a packet from port
128+i to the other three ports; groups 1 and 2 recirculate to 196 (2 makes two copies, rid 1 and
2), which the program's egress tells apart by egress_rid.
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SCHEMA_OPTIONAL = True  # the program is not installed in tofinopd; only $pre entries are written
SOURCES = ["FLM-SpecRegex/simple_cuckoo_firewall_build/lucid.py:init"]
PORTS = [Port(p, "HOST", f"h{p}") for p in (128, 129, 130, 131)]
PORTS += [Port(196, "RECIRC", "recirc"), Port(132, "ATTACKER", "attacker")]
MC_GROUPS = [(640, [(129, 0), (130, 0), (131, 0)]), (641, [(128, 0), (130, 0), (131, 0)]),
             (642, [(128, 0), (129, 0), (131, 0)]), (643, [(128, 0), (129, 0), (130, 0)]),
             (1, [(196, 1)]), (2, [(196, 1), (196, 2)])]


def build(r):
    for mgid, replicas in MC_GROUPS:
        r.multicast_replicas(mgid, replicas)
