"""SwitchV2P: p4-prototype/ptf-tests/test.py setup_env(bfrt_info, 0, SwitchType.TOR), the setup every
TOR test runs (BfRt gRPC through p4-prototype/py/*.py).

swports come from the PTF port map; the default map gives ports 0, 1, 2. The cache registers
(keys/vals/bf) are cleared by the setup, so they keep their declared initial values.
"""
import ipaddress

from cp_snapshot.snapshot import Port

TARGET = "tofino"
SOURCES = ["SwitchV2P/p4-prototype/ptf-tests/test.py:setup_env",
           "SwitchV2P/p4-prototype/py/{l3_forwarding,switch_config,match_gw,"
           "check_misdelivered,mirroring}.py"]
NOTES = ["switch_type TOR (0), switch_id 0, as every TOR test; swports = PTF default 0,1,2"]
SWPORTS = [0, 1, 2]
GW = "132.68.0.3"
# swports[0] and [1] face hosts (132.68.0.2 and the gateway); swports[2] is the mirror port.
PORTS = [Port(0, "HOST", "h0", ip="132.68.0.2"), Port(1, "HOST", "gw", ip=GW),
         Port(2, "LINK", "mirror"), Port(3, "ATTACKER", "attacker")]


def ip(a):
    return int(ipaddress.ip_address(a))


def build(r):
    r.mirror_session(1, SWPORTS[2], "BOTH", 69)
    r.entry("SwitchV2PIngress.fw.l3_forwarding", "l3_forward",
            {"hdr.ipv4_outter.dst_addr": ip("132.68.0.2")}, {"port": SWPORTS[0]})
    r.entry("SwitchV2PIngress.fw.l3_forwarding", "l3_forward",
            {"hdr.ipv4_outter.dst_addr": ip(GW)}, {"port": SWPORTS[1]})
    r.default("SwitchV2PIngress.switch_config", "set_config", {"switch_type": 0, "switch_id": 0})
    r.entry("SwitchV2PIngress.match_gw", "set_to_gw", {"hdr.ipv4_outter.dst_addr": ip(GW)},
            {"to_gw": 1})
    r.entry("SwitchV2PIngress.cache.check_misdelivered", "set_misdelivered",
            {"hdr.ipv4_outter.src_addr": ip("133.68.0.2")}, {"misdelivered": 1})
    r.register_param("SwitchV2PIngress.cache.retransmit_timeout", 8)
