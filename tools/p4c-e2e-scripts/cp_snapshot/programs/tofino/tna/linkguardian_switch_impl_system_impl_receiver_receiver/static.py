"""linkguardian receiver: switch_impl/system_impl/receiver/setup-ae.py (bfrt_python), evaluated
statically by bfrt_python_rules (hostname tofino1a passes the script's host gate; port, TM and
pktgen calls, child processes and the pktgen run_pd_rpc step are device setup and are skipped).

Tables only devtest helpers fill (e.g. link_protect, emulate_random_pkt_drop) are not called by
the setup and stay empty (closed world).
"""
from pathlib import Path

from cp_snapshot.bfrt_python_rules import run_bfrt_python
from cp_snapshot.snapshot import Port

TARGET = "tofino"
ROOT = Path("/root/jiwon/Workspace-remote/top_tier_repo/linkguardian")
SCRIPT = ROOT / "switch_impl/system_impl/receiver/setup-ae.py"
SOURCES = ["linkguardian/switch_impl/system_impl/receiver/setup-ae.py"]
NOTES = ["ENABLE_PFC_ON_LOSS_NOTI=False as shipped: no PFC quanta / loss-notification mcast groups"]
# 36: protected link from the sender switch; 52: next hop (eval topo); 20/44/68: loopback ports.
PORTS = [Port(36, "LINK", "protected_rx"), Port(52, "HOST", "next_hop"),
         Port(20, "RECIRC", "recirc_buff"), Port(44, "RECIRC", "filtering"),
         Port(68, "RECIRC", "recirc_pipe0"), Port(0, "ATTACKER", "attacker")]


def build(r):
    rec = run_bfrt_python(r, SCRIPT, "receiver", hostname="tofino1a",
                          path_map={"/home/sigcomm23ae/linkguardian": str(ROOT)})
    return rec
