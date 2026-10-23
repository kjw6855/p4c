"""lumina inject_switch: lumina/switch/controller/controller.py (BfRt gRPC + PD-thrift port setup).

The orchestrator writes a switch config (forward/mirror/ARP tables, traffic events) from the test's
hosts and starts the controller twice: `-m init` (ports, forwarding, ARP, mirroring, counters) is
the setup; `-m event` waits for the RDMA traffic generator's QP message on listen-port and then
installs the event-injection rules for those QPs (check_first_pkt_in_msg_table,
update_next_roceseq_table, inject_event_table, next_roceseq_reg): runtime (A2S), run here as a
snapshot-sequence STEP that sends the generator's message itself.

The config below follows orchestrator/main.py's layout for a requester (port 60) and a responder
(port 61), each with a mirror host (62, 63); data-packet events as conf/example.yml writes them.
"""
import socket
import subprocess
import time

from cp_snapshot.packages import ControllerStep
from cp_snapshot.snapshot import Port

PROGRAM = "lumina_lumina_switch_p4src_inject_switch"
DESCRIPTION = "controller.py -m init (switch config for a requester/responder pair)"
SOURCE = "lumina/lumina/switch/controller"
PORTS = [Port(60, "HOST", "requester", ip="10.0.0.1"), Port(61, "HOST", "responder", ip="10.0.0.2"),
         Port(62, "LINK", "requester-mirror"), Port(63, "LINK", "responder-mirror"),
         Port(56, "ATTACKER", "attacker")]
LISTEN_PORT = 12345
CONFIG = """\
port-speed: 100G
fec-type: NONE
listen-port: %d
rewrite-udp-dst-port: 9741
forward:
- {dst-mac: '02:00:00:00:00:01', eg-port: 60, host: requester}
- {dst-mac: '02:00:00:00:00:02', eg-port: 61, host: responder}
- {dst-mac: '02:00:00:00:00:03', eg-port: 62, host: requester_mirror}
- {dst-mac: '02:00:00:00:00:04', eg-port: 63, host: responder_mirror}
mirror:
- {direction: ingress_to_egress, src-port: 60, dst-port: 62}
- {direction: ingress_to_egress, src-port: 61, dst-port: 63}
arp:
- {dst-ip: 10.0.0.1, dst-mac: '02:00:00:00:00:01'}
- {dst-ip: 10.0.0.2, dst-mac: '02:00:00:00:00:02'}
traffic:
  num-msgs-per-qp: 1
  num-pkts-per-msg: 4
  data-pkt-events:
  - {qpn: 0, psn: 2, type: drop, iter: 1}
""" % LISTEN_PORT
PATCHES = [
    ("controller.py", "import random\n", "import random\nrandom.seed(0)\n",
     "determinism: mirror session ids come from random.sample; two captures must agree"),
    ("controller.py", "conn.sendall(msg)", "conn.sendall(msg.encode())",
     "bug (Python 3): sendall needs bytes; raised after the event rules were installed"),
]


def _env(ctx):
    return {"PYTHONPATH": f"{ctx.site}:{ctx.site}/tofino:{ctx.site}/p4testutils:{ctx.src}"}


def prepare(ctx):
    (ctx.src / "switch_config.yml").write_text(CONFIG)


def run(ctx):
    rc = ctx.python(["controller.py", "-n", PROGRAM, "-f", "switch_config.yml", "-m", "init"],
                    ctx.src, env=_env(ctx))
    if rc != 0:
        raise RuntimeError(f"controller.py -m init rc={rc}")


def _send_qp_message(ctx, message: str, timeout: float = 120.0) -> None:
    """Run `controller.py -m event` and hand it `message` as the traffic generator would."""
    log = (ctx.work / "step.log").open("a")
    log.write(f"$ controller.py -m event <<< {message}\n")
    log.flush()
    env = dict(ctx.env, **_env(ctx))
    proc = subprocess.Popen(
        ["python3", "controller.py", "-n", PROGRAM, "-f", "switch_config.yml", "-m", "event",
         "-s", str(ctx.work / "event_snapshot.yml")],
        cwd=ctx.src, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.time() + timeout
        while True:  # the controller listens once its BfRt binding is up
            try:
                with socket.create_connection(("127.0.0.1", LISTEN_PORT), timeout=5) as s:
                    s.sendall(message.encode())
                    s.recv(4096)  # the controller echoes the message once the rules are in
                break
            except OSError:
                if proc.poll() is not None or time.time() > deadline:
                    raise RuntimeError("controller.py -m event never accepted the QP message "
                                       "(see step.log)")
                time.sleep(1)
        rc = proc.wait(timeout=timeout)
        if rc != 0:
            raise RuntimeError(f"controller.py -m event rc={rc} (see step.log)")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        log.close()


def _event(ctx):
    """One WRITE connection: requester QPN 17 PSN 100 (10.0.0.1), responder QPN 18 PSN 200
    (10.0.0.2). parse_msg takes the first decimal number of each field and the IP from the GID."""
    _send_qp_message(ctx, "WRITE;1;17,100,::ffff:10.0.0.1;18,200,::ffff:10.0.0.2&")


def _event2(ctx):
    """A second test run: new QPs (QPN 33/34) replace the first run's rules (configure_injection
    deletes every entry of its three tables first)."""
    _send_qp_message(ctx, "WRITE;1;33,500,::ffff:10.0.0.1;34,600,::ffff:10.0.0.2&")


STEPS = {
    "event": ControllerStep(_event, "controller.py -m event + QP message WRITE 17/18",
                            registers=("next_roceseq_reg",)),
    "event2": ControllerStep(_event2, "controller.py -m event + QP message WRITE 33/34",
                             registers=("next_roceseq_reg",)),
}
SCENARIOS = {"event": ["event"], "event_twice": ["event", "event2"]}
