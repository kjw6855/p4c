"""p4control: switch/controller.py (BfRt gRPC; the program name is auto-detected), up to its
print("DONE"). After that it loops forever on digests, adding table_conn rows per connection:
runtime (A2S), not part of the post-setup state. capture.py stops it at DONE; validate_live.py
keeps the loop running while the case's packets go out.

netcl.py, which controller.py imports for the policy rules, does not parse (`def insert_rules:`)
and refers to controller.py's globals; both are fixed minimally, the rules themselves untouched.
"""
from cp_snapshot.packages import ControllerStep
from cp_snapshot.snapshot import Port

DESCRIPTION = "switch/controller.py up to print('DONE'); digest loop not run"
SOURCE = "p4control/switch"
# table_forward: 10.0.0.1 -> 60, 10.0.0.2 -> 61, 10.0.0.3 -> 62.
PORTS = [Port(60, "HOST", "h1", ip="10.0.0.1"), Port(61, "HOST", "h2", ip="10.0.0.2"),
         Port(62, "HOST", "h3", ip="10.0.0.3"), Port(63, "ATTACKER", "attacker")]
PATCHES = [
    ("netcl.py", "def insert_rules:", "def insert_rules():",
     "bug: SyntaxError (missing parameter list)"),
    ("controller.py", "netcl.insert_rules()", "netcl.__dict__.update(globals()); netcl.insert_rules()",
     "bug: insert_rules uses controller.py's tables, target, gc, struct, socket"),
    ("controller.py", "    tag_id = data_dict['tag_id']", "    tag_id = data_dict.get('tag_id')",
     "bug: digest_t has no tag_id; the KeyError (swallowed) dropped every digest"),
    ("controller.py", "    block_dst_addr = data_dict['block_dst_addr']",
     "    block_dst_addr = data_dict.get('block_dst_addr')", "bug: digest_t has no block_dst_addr"),
    ("controller.py", "    tag_decision = data_dict['tag_decision']",
     "    tag_decision = data_dict.get('tag_decision')", "bug: digest_t has no tag_decision"),
]
SETUP_ONLY_PATCHES = [
    ("controller.py", 'print("DONE")', 'print("DONE"); interface.tear_down_stream(); sys.exit(0)',
     "stop before the digest loop (runtime table_conn updates)"),
]
READY_MARKER = "DONE"


def run(ctx):
    rc = ctx.python(["controller.py"], ctx.src)
    if rc != 0:
        raise RuntimeError(f"controller.py rc={rc}")


def _digest(ctx):
    """h1 opens a connection to h2 with ipv4.flags[15] set (the reserved bit): the data plane
    digests it (conn_add, DIGEST_ADD_CONN) and the controller's loop adds one table_conn row."""
    from scapy.all import IP, TCP, Ether
    pkt = Ether() / IP(src="10.0.0.1", dst="10.0.0.2", flags="evil") / TCP(sport=1234, dport=80)
    ctx.send(60, bytes(pkt))


STEPS = {"digest": ControllerStep(_digest, "h1 -> h2 TCP 1234->80 with ipv4.flags[15]=1 "
                                           "(digest: controller adds a table_conn row)")}
SCENARIOS = {"digest": ["digest"]}
