#!/usr/bin/env python3
"""Drive the ALREADY-RUNNING (warm) simple_switch_grpc on 9559/9090 — avoids the
harness startup race. For one NdN H2S2K txtpb:
  A) reset + install txtpb entities, read pit_r[0], send ONLY the Phase-2 attacker
     packet, re-read pit_r[0]  (does the attacker actually tamper the register?)
  B) full differential: legit (P1->P3) vs attack (P1->P2->P3); print verdict.
"""
import sys, time
from pathlib import Path

sys.argv = ["ndn_probe2"]
import tampering as T
T.load_bmv2_deps()

P4INFO = "/tmp/ndn_harness/out/P4LTL_NdN_main/build/P4LTL_NdN_main_p4info.txt"
JSON = "/tmp/ndn_harness/out/P4LTL_NdN_main/build/P4LTL_NdN_main.json"
TXTPB = Path(sys.argv[1] if len(sys.argv) > 1 else
             "/tmp/v1model_e2e_20260703_053323/gen/P4LTL_NdN_main/protobuf/STATE_DEP_TAMPERING/main_1_1.txtpb")
CAP = Path("/tmp/ndn_probe/pcap"); CAP.mkdir(parents=True, exist_ok=True)


def pit0(thrift):
    v = thrift.read_all("pit_r")
    return v[0] if v else None


def main():
    case = T.parse_tampering_txtpb(TXTPB)
    print(f"txtpb: {TXTPB.name}  phases ports {[p.in_port for p in case.phases]}  "
          f"entities {[e.table_entry.table_id for e in case.entities]}", flush=True)

    c = T.P4RuntimeClient(device_id=T.DEVICE_ID, grpc_address="127.0.0.1:9559",
                          election_id=T.ELECTION_ID)
    th = T.Bmv2ThriftClient("127.0.0.1", 9090)
    c.set_fwd_pipe_config(P4INFO, JSON)
    p4info = T.load_p4info(c)
    const_ids = T.const_table_ids(p4info)
    tester = T.PacketTester(1.5, CAP, thrift_client=th)

    def reset_install():
        T.clear_all_entries(c, p4info)
        T.clear_all_registers(c, p4info, thrift_client=th)
        T.install_entities(c, case.entities, const_ids)

    print("\n=== A: does Phase-2 attacker packet tamper pit_r? ===", flush=True)
    reset_install()
    print(f"  pit_r[0] baseline: {pit0(th)}", flush=True)
    out2 = tester._send_capture(case.phases[1], "A_p2")
    print(f"  after sending P2 (port {case.phases[1].in_port}); out="
          f"{'drop' if out2 is None else f'port {out2[0]}'}", flush=True)
    print(f"  pit_r[0] AFTER P2: {pit0(th)}   (nonzero => register tampered)", flush=True)

    print("\n=== B: differential legit(P1->P3) vs attack(P1->P2->P3) ===", flush=True)
    reset_install()
    legit, _ = tester.replay(case, [0, 2], label="B_legit")
    reset_install()
    attack, _ = tester.replay(case, [0, 1, 2], label="B_attack")
    ok, reason = tester.compare_runs(legit, attack)
    d = lambda o: "drop" if o is None else f"port {o[0]},{len(o[1])}B"
    print(f"  legit  P3: {d(legit)}", flush=True)
    print(f"  attack P3: {d(attack)}", flush=True)
    print(f"  VERDICT: {'VULNERABLE' if ok else 'no divergence'} — {reason}", flush=True)

    c.tear_down(); th.close()


if __name__ == "__main__":
    main()
