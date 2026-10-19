"""fisslock: switch/control/bfrt.c driver_init() (C BfRt + bf_mc), machines from cluster.h.

eth_fallback maps each machine's MAC to its port. Multicast groups per machine i (MID = i+1):
MID (rid 1, agent notification) and MID+128 (rid 2, grant reply) reach machine i only; MID+192
(rid 0) reaches every other machine; 255 (rid 0) reaches all. Egress branches on the rid.
"""
from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = ["fisslock/switch/control/bfrt.c:driver_init,setup_mgrps", "fisslock/switch/control/cluster.h"]
# (name, ipv4, mac, port) as in cluster.h
MACHINES = [("pro0-1", 0x0a000204, 0x1070fd0de230, 24), ("pro1-1", 0x0a000201, 0x08c0ebfd52c4, 16),
            ("pro2-1", 0x0a000202, 0x08c0ebfe8c50, 4), ("pro3-1", 0x0a000203, 0x1070fd0de448, 12),
            ("pro0-2", 0x0a000205, 0x08c0ebde0156, 56), ("pro1-2", 0x0a000206, 0x08c0ebdcb30a, 48),
            ("pro2-2", 0x0a000207, 0x08c0ebde011a, 40), ("pro3-2", 0x0a000208, 0x08c0ebdca15a, 32)]
# Every machine is a host; port 0 is unused by the cluster.
PORTS = [Port(p, "HOST", n, mac=f"{m:012x}", ip=f"10.0.2.{ip & 0xff}") for n, ip, m, p in MACHINES]
PORTS.append(Port(0, "ATTACKER", "attacker"))


def build(r):
    for _, _, mac, port in MACHINES:
        r.entry("IngressPipe.eth_fallback", "eth_forward", {"dst_mac": mac}, {"port": port})
    ports = [p for _, _, _, p in MACHINES]
    for i, (_, _, _, port) in enumerate(MACHINES):
        mid = i + 1
        r.multicast_group(mid, [port], rid=1)
        r.multicast_group(mid + 128, [port], rid=2)
        r.multicast_group(mid + 192, [p for p in ports if p != port], rid=0)
    r.multicast_group(255, ports, rid=0)
