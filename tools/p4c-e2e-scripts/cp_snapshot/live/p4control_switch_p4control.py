"""p4control: switch/controller.py (BfRt gRPC; the program name is auto-detected), up to its
print("DONE"). After that it loops forever on digests, adding table_conn rows per connection:
runtime (A2S), not part of the post-setup state.

netcl.py, which controller.py imports for the policy rules, does not parse (`def insert_rules:`)
and refers to controller.py's globals; both are fixed minimally, the rules themselves untouched.
"""
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
    ("controller.py", 'print("DONE")', 'print("DONE"); interface.tear_down_stream(); sys.exit(0)',
     "stop before the digest loop (runtime table_conn updates)"),
]


def run(ctx):
    rc = ctx.python(["controller.py"], ctx.src)
    if rc != 0:
        raise RuntimeError(f"controller.py rc={rc}")
