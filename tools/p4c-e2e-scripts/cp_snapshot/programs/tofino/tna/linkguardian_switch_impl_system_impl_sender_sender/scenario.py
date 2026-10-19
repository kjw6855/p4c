"""linkguardian sender: system_impl/sender/setup-ae.py in bfshell (the AE setup), as on the
authors' testbed host, with the environment patched for tofino-model.

Kept as written: port adds, loopback modes and TM queue priorities (accepted by the model), every
table/mirror/PRE/register write, and the devtest_cmds.py exec (definitions only). Skipped: the
scapy injection of the initial sender packets and the run_pd_rpc pktgen configuration (traffic and
pktgen are not part of the post-setup state; run_pd_rpc is not in open-p4studio).
"""
from cp_snapshot.snapshot import Port

DESCRIPTION = "sender/setup-ae.py (bfrt_python, bfshell -b); traffic injection and pktgen skipped"
SOURCE = "linkguardian/switch_impl/system_impl"
PORTS = [Port(32, "LINK", "protected_tx"), Port(16, "RECIRC", "recirc_buff"),
         Port(68, "RECIRC", "recirc_pipe0"), Port(36, "HOST", "lumos"), Port(60, "LINK", "reporting"),
         Port(0, "HOST", "host"), Port(1, "ATTACKER", "attacker")]
_R = "sender"
_AE = "/home/sigcomm23ae/linkguardian/switch_impl/system_impl"
PATCHES = [
    (f"{_R}/setup-ae.py", 'if hostname == "tofino1a" or hostname == "tofino1c":',
     "if True:  # (capture) host gate", "host gate: the AE testbed hostnames"),
    (f"{_R}/setup-ae.py", _AE, "{SRC}", "paths: the AE account's checkout"),
    (f"{_R}/setup-ae.py", "output = subprocess.check_output(['sudo', python3_scapy_interpreter, init_pkt_injection_script, 'dummy', '32'])",
     'output = b"(capture) initial packet injection skipped"',
     "traffic: scapy injection on the testbed NIC (packets would update registers)"),
    (f"{_R}/setup-ae.py", 'os.system("$SDE/run_pd_rpc.py {}".format(config_pktgen_script))',
     'print("(capture) run_pd_rpc pktgen config skipped")',
     "pktgen config through run_pd_rpc (not in open-p4studio; hardware traffic generator)"),

] + [(f"{_R}/{f}", "bfrt.sender.", "bfrt.linkguardian_switch_impl_system_impl_sender_sender.",
      "program name: the authors compiled it as sender") for f in ("setup-ae.py", "devtest_cmds.py")]


def run(ctx):
    out = ctx.bfshell(ctx.src / _R / "setup-ae.py", cwd=ctx.src / _R)
    if "Traceback" in out or "devtest_cmds.py loaded!" not in out:
        raise RuntimeError("setup-ae.py did not complete (see controller.log)")
