#!/usr/bin/env python3
"""Focused probe: does the NdN Phase-2 attacker packet actually tamper pit_r on real bmv2?

Reuses tampering.py's bmv2 machinery. For one NdN H2S2K txtpb it:
  1. resets state + installs the txtpb entities (count_table + pit_table only),
  2. reads pit_r[0] baseline,
  3. sends ONLY the Phase-2 attacker packet,
  4. re-reads pit_r[0]  <-- the decisive measurement,
  5. runs the full differential (legit P1->P3 vs attack P1->P2->P3) and prints the verdict.
"""
import sys
from pathlib import Path

# Capture the intended txtpb path BEFORE we blank sys.argv for tampering's argparse.
_TXTPB_ARG = sys.argv[1] if len(sys.argv) > 1 else \
    "/tmp/ndn_regen/out/P4LTL_NdN_main/protobuf/STATE_DEP_TAMPERING/main_1_1.txtpb"
sys.argv = ["ndn_probe"]  # keep tampering's argparse out of the way on import
import tampering as T

T.load_bmv2_deps()

JSON = Path("/tmp/ndn_probe/ndn_small.json")
P4INFO = Path("/tmp/ndn_probe/ndn_small_p4info.txt")
TXTPB = Path(_TXTPB_ARG)
CAP = Path("/tmp/ndn_probe/pcap")
CAP.mkdir(parents=True, exist_ok=True)


def pit0(thrift):
    vals = thrift.read_all("pit_r")
    return vals[0] if vals else None


def main():
    case = T.parse_tampering_txtpb(TXTPB)
    print(f"txtpb: {TXTPB.name}")
    print(f"  phases: ports {[ph.in_port for ph in case.phases]}  "
          f"lens {[len(ph.in_packet) for ph in case.phases]}")
    print(f"  entities (table_id): {[e.table_entry.table_id for e in case.entities]}")
    print(f"  affected_registers: {case.affected_registers}")

    def step(msg):
        print(f"  [step] {msg}", flush=True)

    with T.bmv2_session(JSON, Path('/tmp/ndn_probe/bmv2.log'), keep=False):
        step("bmv2 up; connecting P4Runtime")
        client = T.P4RuntimeClient(device_id=T.DEVICE_ID,
                                   grpc_address=f"{T.GRPC_HOST}:{T.GRPC_PORT}",
                                   election_id=T.ELECTION_ID)
        step("connecting Thrift")
        thrift = T.Bmv2ThriftClient(T.GRPC_HOST, T.THRIFT_PORT)
        step("set_fwd_pipe_config")
        client.set_fwd_pipe_config(str(P4INFO), str(JSON))
        step("load_p4info")
        p4info = T.load_p4info(client)
        const_ids = T.const_table_ids(p4info)
        tester = T.PacketTester(1.5, CAP, thrift_client=thrift)
        step("setup done")

        def reset_install():
            T.clear_all_entries(client, p4info)
            T.clear_all_registers(client, p4info, thrift_client=thrift)
            T.install_entities(client, case.entities, const_ids)

        # ---- Measurement A: does P2 alone write pit_r? ----
        print("\n=== Measurement A: P2 attacker write ===")
        reset_install()
        print(f"  pit_r[0] baseline (after reset+install): {pit0(thrift)}")
        out2 = tester._send_capture(case.phases[1], "A_p2")
        print(f"  sent P2 (attacker, port {case.phases[1].in_port}); switch out: "
              f"{'drop' if out2 is None else f'port {out2[0]}, {len(out2[1])}B'}")
        print(f"  pit_r[0] AFTER P2: {pit0(thrift)}   <-- nonzero => register tampered")

        # ---- Measurement B: full differential oracle ----
        print("\n=== Measurement B: differential (legit P1->P3 vs attack P1->P2->P3) ===")
        reset_install()
        legit, _ = tester.replay(case, [0, 2], label="B_legit")
        reset_install()
        attack, _ = tester.replay(case, [0, 1, 2], label="B_attack")
        ok, reason = tester.compare_runs(legit, attack)
        print(f"  legit  P3 out: {'drop' if legit is None else f'port {legit[0]}, {len(legit[1])}B'}")
        print(f"  attack P3 out: {'drop' if attack is None else f'port {attack[0]}, {len(attack[1])}B'}")
        print(f"  verdict: {'VULNERABLE' if ok else 'no divergence'}  --  {reason}")

        client.tear_down()
        thrift.close()


if __name__ == "__main__":
    main()
