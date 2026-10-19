"""fisslock: switch/control (main.c + bfrt.c), the C BfRt controller that embeds the switch driver
(bf_switchd_lib_init) and runs driver_init(): eth_fallback rules and the multicast groups for the
machines of cluster.h.

Built here against open-p4studio's libdriver.so (the README builds it inside the bf-drivers
automake tree; the SDE moved bfutils/clish and bfsys headers to target-utils/target-sys, so two
one-line shim headers map the old include paths; and its BfRt C API is the BFRT_GENERIC_FLAGS
variant, so entry add/mod get flags = 0: without it the arguments shift and the add segfaults). Not run: port-setup.bfsh (100G port adds).
main() returns right after the setup, which would discard the state; it waits instead.

The multicast groups are created with the raw bf_mc (mc_mgr) API: every call returns BF_SUCCESS,
but BfRt's $pre.mgid/$pre.node read back nothing for them on open-p4studio (from_hw true or
false). They are supplemented from the static transcription of the same setup_mgrps() code.
"""
import subprocess
import time

from cp_snapshot.capture import SDE_INSTALL
from cp_snapshot.snapshot import Port

DESCRIPTION = "switch/control fisslock_decider (C BfRt, embedded switchd) driver_init()"
SOURCE = "fisslock/switch/control"
MACHINES = [("pro0-1", 0x0a000204, 0x1070fd0de230, 24), ("pro1-1", 0x0a000201, 0x08c0ebfd52c4, 16),
            ("pro2-1", 0x0a000202, 0x08c0ebfe8c50, 4), ("pro3-1", 0x0a000203, 0x1070fd0de448, 12),
            ("pro0-2", 0x0a000205, 0x08c0ebde0156, 56), ("pro1-2", 0x0a000206, 0x08c0ebdcb30a, 48),
            ("pro2-2", 0x0a000207, 0x08c0ebde011a, 40), ("pro3-2", 0x0a000208, 0x08c0ebdca15a, 32)]
PORTS = [Port(p, "HOST", n, mac=f"{m:012x}", ip=f"10.0.2.{ip & 0xff}") for n, ip, m, p in MACHINES]
PORTS.append(Port(0, "ATTACKER", "attacker"))
PATCHES = [
    ("bfrt.c", "bfrt_session, &bfrt_target, tbl.tbl_key, tbl.tbl_data);",
     "bfrt_session, &bfrt_target, 0, tbl.tbl_key, tbl.tbl_data);",
     "SDE API: open-p4studio is built with BFRT_GENERIC_FLAGS (entry add/mod take flags)"),
    ("bfrt.c", "bfrt_session, &bfrt_target, table.tbl_key, table.tbl_data);",
     "bfrt_session, &bfrt_target, 0, table.tbl_key, table.tbl_data);",
     "SDE API: open-p4studio is built with BFRT_GENERIC_FLAGS (entry add/mod take flags)"),
    ("bfrt.h", '"fisslock_decider"', '"fisslock_switch_p4_switch"',
     "program name: the authors compiled the P4 program as fisslock_decider"),
    ("main.c", "  free(switchd_ctx);\n  return 0;",
     '  printf("(capture) setup done\\n");\n  fflush(stdout);\n  while (1) pause();',
     "keep the process (and so the switch state) up for the capture instead of exiting"),
]
SHIMS = {"bfutils/clish/thread.h": "target-utils/clish/thread.h",
         "bfsys/bf_sal/bf_sys_intf.h": "target-sys/bf_sal/bf_sys_intf.h"}


def prepare(ctx):
    for old, new in SHIMS.items():
        f = ctx.src / "shim" / old
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(f"#include <{new}>\n")
    inc = f"{SDE_INSTALL}/include"
    cmd = ["gcc", "-DBFRT_GENERIC_FLAGS", "-O1", "-w", "main.c", "bfrt.c", "-Ishim", f"-I{inc}", f"-I{inc}/target-sys",
           f"-I{inc}/target-utils", f"-L{SDE_INSTALL}/lib", "-Wl,--no-as-needed", "-ldriver",
           "-lbfutils", "-ltarget_sys", "-ltarget_utils", "-lpthread", "-lm", "-ldl",
           f"-Wl,-rpath,{SDE_INSTALL}/lib", "-o", "fisslock_decider"]
    r = subprocess.run(cmd, cwd=ctx.src, capture_output=True, text=True)
    (ctx.work / "build.log").write_text(" ".join(cmd) + "\n" + r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError("fisslock_decider build failed (see build.log)")
    ctx.note("fisslock_decider built against open-p4studio libdriver.so (shim headers)")


def switchd_argv(ctx):
    return [str(ctx.src / "fisslock_decider"), "--install-dir", SDE_INSTALL, "--conf-file",
            f"{SDE_INSTALL}/share/p4/targets/tofino/fisslock_switch_p4_switch.conf"]


def run(ctx):
    log = ctx.work / "controller_switchd.log"
    deadline = time.time() + 600
    while time.time() < deadline:
        if log.exists() and "(capture) setup done" in log.read_text(errors="replace"):
            return
        time.sleep(2)
    raise RuntimeError("fisslock_decider did not finish driver_init (see controller_switchd.log)")


def supplement(ctx):
    from cp_snapshot.compare import static_entities
    pre = [e for e in static_entities(ctx.program) if e.table.startswith("$pre.")]
    ctx.note(f"$pre from static/{ctx.program}.py (setup_mgrps: {len(pre) // 2} groups); "
             "bf_mc groups are not visible through BfRt")
    return pre
