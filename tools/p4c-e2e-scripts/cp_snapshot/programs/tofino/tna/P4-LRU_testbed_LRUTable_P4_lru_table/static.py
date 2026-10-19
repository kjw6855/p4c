"""P4-LRU LRUTable: testbed/LRUTable/P4/set_lru_table.py (bfrt_python).

send_table forwards between the two testbed ports; every other call in the script clears a register
(their cells stay at the declared initial value, so nothing else is captured).
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = ["P4-LRU/testbed/LRUTable/P4/set_lru_table.py"]
# Hosts are the two ports send_table connects; 136 is unused on the same pipe.
PORTS = [Port(128, "HOST", "h128"), Port(144, "HOST", "h144"), Port(136, "ATTACKER", "attacker")]


def build(r):
    r.entry("Ingress1.send_table", "send", {"ingress_port": 128}, {"port": 144})
    r.entry("Ingress1.send_table", "send", {"ingress_port": 144}, {"port": 128})
