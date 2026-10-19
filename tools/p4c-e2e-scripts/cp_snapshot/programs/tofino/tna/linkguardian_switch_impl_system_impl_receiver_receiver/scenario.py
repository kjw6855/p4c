"""linkguardian receiver: system_impl/receiver/setup-ae.py in bfshell (the AE setup), as on the
authors' testbed host, with the environment patched for tofino-model.

Kept as written: port adds, loopback modes and TM queue priorities (accepted by the model), every
table/mirror/PRE/register write, and the devtest_cmds.py exec (definitions only). Skipped: the
scapy injection of the initial receiver packets and the run_pd_rpc pktgen configuration (traffic and
pktgen are not part of the post-setup state; run_pd_rpc is not in open-p4studio).
"""
from cp_snapshot.snapshot import Port

DESCRIPTION = "receiver/setup-ae.py (bfrt_python, bfshell -b); traffic injection and pktgen skipped"
SOURCE = "linkguardian/switch_impl/system_impl"
PORTS = [Port(36, "LINK", "protected_rx"), Port(52, "HOST", "next_hop"),
         Port(20, "RECIRC", "recirc_buff"), Port(44, "RECIRC", "filtering"),
         Port(68, "RECIRC", "recirc_pipe0"), Port(0, "ATTACKER", "attacker")]
_R = "receiver"
_AE = "/home/sigcomm23ae/linkguardian/switch_impl/system_impl"
PATCHES = [
    (f"{_R}/setup-ae.py", 'if hostname == "tofino1a" or hostname == "tofino1c":',
     "if True:  # (capture) host gate", "host gate: the AE testbed hostnames"),
    (f"{_R}/setup-ae.py", _AE, "{SRC}", "paths: the AE account's checkout"),
    (f"{_R}/setup-ae.py", "output = subprocess.check_output(['sudo', python3_scapy_interpreter, init_pkt_injection_script, 'courier', '36'])",
     'output = b"(capture) initial packet injection skipped"',
     "traffic: scapy injection on the testbed NIC (packets would update registers)"),
    (f"{_R}/setup-ae.py", 'os.system("$SDE/run_pd_rpc.py {}".format(config_pktgen_script))',
     'print("(capture) run_pd_rpc pktgen config skipped")',
     "pktgen config through run_pd_rpc (not in open-p4studio; hardware traffic generator)"),
    (f"{_R}/setup-ae.py", "bfrt.tf1.pktgen.app_cfg.mod_with_trigger_timer_periodic(app_id=1, app_enable=True)",
     "print(\"(capture) pktgen app 1 not enabled\")",
     "traffic: pktgen ACK-timeout packets would update registers during capture"),
] + [(f"{_R}/{f}", "bfrt.receiver.", "bfrt.linkguardian_switch_impl_system_impl_receiver_receiver.",
      "program name: the authors compiled it as receiver") for f in ("setup-ae.py", "devtest_cmds.py")]


def run(ctx):
    out = ctx.bfshell(ctx.src / _R / "setup-ae.py", cwd=ctx.src / _R)
    if "Traceback" in out or "devtest_cmds.py loaded!" not in out:
        raise RuntimeError("setup-ae.py did not complete (see controller.log)")
