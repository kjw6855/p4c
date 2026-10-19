"""netchain, switch s1 (the chain head, role 100).

topo.py:config_mininet feeds config/commands.txt to s1, then run_test.py:init_flowtable sends s1
the per-key rows for numKeyValue=120 keys (config.xml): get_my_address, find_index, value_reg.
The original is Python 2 + mininet; here the same command stream goes to simple_switch_CLI
(init_flowtable's string building is reproduced below, its send replaced). fail_recovery.py
is runtime (A2S) and not run.

s1 links: eth1 -> s4 (where the hosts attach), eth2 -> s2 (topo.txt).
"""
from cp_snapshot.snapshot import Port

TARGET = "bmv2"
DESCRIPTION = "s1: config/commands.txt + run_test.py init_flowtable (num_kv=120)"
SOURCE = "netchain-p4/controller"
PORTS = [Port(1, "HOST", "to-s4-hosts"), Port(2, "LINK", "to-s2"), Port(3, "ATTACKER", "attacker")]


def init_flowtable_s1(num_kv: int) -> str:
    """run_test.py:init_flowtable for switch_id 0, verbatim apart from the send."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("nc_config", SRC / "nc_config.py")
    nc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nc)
    role = [100, 101, 102]
    switch_id = 0
    switch_ip = nc.IP_PREFIX + str(switch_id + 1)
    init_cmd = ""
    for i in range(num_kv):
        key = nc.ENTRY[1][i]
        init_cmd += ("table_add get_my_address get_my_address_act " + str(key) + " => "
                     + switch_ip + " " + str(role[switch_id]) + "\n")
        init_cmd += "table_add find_index find_index_act " + str(key) + " => " + str(i) + "\n"
        init_cmd += "register_write value_reg " + str(i) + " " + str(i) + "\n"
    return init_cmd


SRC = None


def run(ctx):
    global SRC
    SRC = ctx.src
    import xml.etree.ElementTree as ET
    num_kv = int(ET.parse(ctx.src / "config/config.xml").getroot().find("numKeyValue").text)
    ctx.note("python2/mininet controller replaced by a shim: same CLI command stream to s1")
    ctx.cli((ctx.src / "config/commands.txt").read_text())
    ctx.cli(init_flowtable_s1(num_kv))
