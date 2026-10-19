"""linkguardian sender: switch_impl/system_impl/sender/setup-ae.py (bfrt_python), evaluated
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
SCRIPT = ROOT / "switch_impl/system_impl/sender/setup-ae.py"
SOURCES = ["linkguardian/switch_impl/system_impl/sender/setup-ae.py"]
NOTES = ["link_protect is filled only by devtest enable_protection(), so protection stays off"]
# 32: protected link to the receiver; 16: recirculation buffer port.
PORTS = [Port(32, "LINK", "protected_tx"), Port(16, "RECIRC", "recirc_buff"),
         Port(0, "HOST", "host"), Port(1, "ATTACKER", "attacker")]


def build(r):
    rec = run_bfrt_python(r, SCRIPT, "sender", hostname="tofino1a",
                          path_map={"/home/sigcomm23ae/linkguardian": str(ROOT)})
    return rec
