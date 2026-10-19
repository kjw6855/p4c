"""FLM-SpecRegex simple_cuckoo_firewall: lucid.py in bfshell (p4tapp.sh: run_bfshell.sh -b lucid.py),
the Lucid-generated control plane. On tofino-model it leaves the ports alone (ports_up defaults to
"model not running") and adds the multicast groups through the C driver (libs/controldriver.py).

controldriver only accepts an SDE whose install tree has a "*.manifest" naming bf-drivers >= 9.5;
open-p4studio has none, so the version check is bypassed (environment patch).
"""
from cp_snapshot.snapshot import Port

DESCRIPTION = "lucid.py (Lucid controldriver in bfshell -b); multicast groups only on the model"
SOURCE = "FLM-SpecRegex/simple_cuckoo_firewall_build"
PORTS = [Port(p, "HOST", f"h{p}") for p in (128, 129, 130, 131)]
PORTS += [Port(196, "RECIRC", "recirc"), Port(132, "ATTACKER", "attacker")]
PATCHES = [
    ("libs/controldriver.py", "    sde_supported = check_sde_version(sde_ver)",
     "    sde_supported = True  # (capture) open-p4studio ships no bf-drivers *.manifest",
     "SDE version gate: open-p4studio's install has no bf-drivers manifest to read"),
]


def run(ctx):
    out = ctx.bfshell(ctx.src / "lucid.py")
    if "control startup complete." not in out:
        raise RuntimeError("lucid.py did not complete (see controller.log)")
