/*
 * Search-optimization fixture (TNA) for --shared-traversal / --chain-impact-order.
 *
 * Redesigned to the accumulation/RMW shape that actually EMITS (mirrors accum_threshold_tna.p4):
 * a counter is incremented once per packet inside a RegisterAction and gated by a constant threshold,
 * and the threshold-exceeded flag feeds an if-condition sink (H2S2C). A bare `if(reg==k)` on a plain
 * read does not emit — its Phase-3 condition is unresolved — so all three emitting chains use the
 * RMW+threshold+table-sink form instead.
 *
 * Four H2S2C chains (regA..regD), shaped so each search option has a distinct, observable effect:
 *
 *  Property 1 (commit 1 — shared Phase-1): regA..regC and regD's read all execute on ONE path, so a
 *    single Phase-1 read terminal buckets into all four chains.
 *      --shared-traversal=PHASE1 -> "Shared Phase-1: 4 chains, N examined, >N bucketed"
 *      --shared-traversal=NONE   -> one "Per-chain Phase-1" pass per chain, same emitted txtpb.
 *
 *  Property 2 (commit 2 — Phase-2 prefilter): regD's WRITE (set_d) is guarded by the contradiction
 *    hdr.ipv4.identification == 1 && == 2, so it is unreachable; its READ (get_d) is reachable, so
 *    Phase 1 still buckets it.
 *      --shared-traversal=PHASE1_PHASE2 -> "Phase-2 prefilter: 4 chains, 1 pruned"; regD skipped.
 *      --shared-traversal=PHASE1        -> regD churns allCovered=false, rejected after the full loop.
 *
 *  Property 3 (commit 3 — impact order): only regA's sink reaches an enforcement primitive (drop_tbl),
 *    regB/regC set metadata only. --chain-impact-order ranks regA first of four.
 *
 * The index is header-sourced so the RegisterAction chains anchor (a constant index does not — see
 * the RegisterAction data-path gap). Generation-only (no Tofino HW replay on this machine).
 *
 * Expected (tofino/tna, --path-selection STATE_DEP_TAMPERING_COND): >=1 emitted txtpb, identical
 * across NONE / PHASE1 / PHASE1_PHASE2 and with/without --chain-impact-order.
 */
#include <core.p4>
#include <tna.p4>

header ethernet_h { bit<48> dst_addr; bit<48> src_addr; bit<16> ether_type; }
header ipv4_h {
    bit<8>  version_ihl; bit<8>  diffserv;  bit<16> total_len;  bit<16> identification;
    bit<16> flags_frag;  bit<8>  ttl;       bit<8>  protocol;   bit<16> hdr_checksum;
    bit<32> src_addr;    bit<32> dst_addr;
}

struct headers_t { ethernet_h ethernet; ipv4_h ipv4; }
struct ig_metadata_t {
    bit<1> exc_a; bit<1> exc_b; bit<1> exc_c; bit<8> d_val;
    bit<1> flag_b; bit<1> flag_c; bit<1> flag_d;
}
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        transition parse_ethernet;
    }
    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select (hdr.ethernet.ether_type) { 0x0800: parse_ipv4; default: accept; }
    }
    state parse_ipv4 { pkt.extract(hdr.ipv4); transition accept; }
}

control Ingress(inout headers_t hdr, inout ig_metadata_t ig_md,
                in    ingress_intrinsic_metadata_t              ig_intr_md,
                in    ingress_intrinsic_metadata_from_parser_t  ig_prsr_md,
                inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                inout ingress_intrinsic_metadata_for_tm_t       ig_tm_md) {

    // regA..regC: increment-only RMW gated by a constant threshold (C=20 -> the tamper is realized by
    // accumulation, driveRegisterPhase2 solves k and emits repeat_count). All header-indexed.
    Register<bit<32>, bit<8>>(256, 0) regA;
    Register<bit<32>, bit<8>>(256, 0) regB;
    Register<bit<32>, bit<8>>(256, 0) regC;
    // regD: split read/write so the write can be made unreachable while the read still buckets.
    Register<bit<8>,  bit<8>>(256, 0) regD;

    RegisterAction<bit<32>, bit<8>, bit<1>>(regA) bump_a = {
        void apply(inout bit<32> v, out bit<1> exc) { exc = 0; if (v > 20) exc = 1; v = v + 1; }
    };
    RegisterAction<bit<32>, bit<8>, bit<1>>(regB) bump_b = {
        void apply(inout bit<32> v, out bit<1> exc) { exc = 0; if (v > 20) exc = 1; v = v + 1; }
    };
    RegisterAction<bit<32>, bit<8>, bit<1>>(regC) bump_c = {
        void apply(inout bit<32> v, out bit<1> exc) { exc = 0; if (v > 20) exc = 1; v = v + 1; }
    };
    RegisterAction<bit<8>, bit<8>, bit<8>>(regD) get_d = {
        void apply(inout bit<8> v, out bit<8> r) { r = v; }
    };
    RegisterAction<bit<8>, bit<8>, bit<8>>(regD) set_d = {
        void apply(inout bit<8> v) { v = hdr.ipv4.ttl; }
    };

    action drop() { ig_dprsr_md.drop_ctl = 0x1; }
    table drop_tbl {
        actions = { drop; NoAction; }
        default_action = drop();
        size = 1;
    }

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;   // header-sourced index: anchors all four chains
            // ---- four reads on ONE path: one Phase-1 terminal buckets into four chains ----
            ig_md.exc_a = bump_a.execute(idx);
            ig_md.exc_b = bump_b.execute(idx);
            ig_md.exc_c = bump_c.execute(idx);
            ig_md.d_val = get_d.execute(idx);
            // regD's write is UNREACHABLE: identification cannot be both 1 and 2. Only a Phase-2
            // write-path pass can prove this cheaply; Phase 1 still buckets regD via its reachable read.
            if (hdr.ipv4.identification == 1 && hdr.ipv4.identification == 2) {
                set_d.execute(idx);
            }
            // Sink A reaches an enforcement primitive (drop) -> the single class-A chain.
            if (ig_md.exc_a == 1) {
                drop_tbl.apply();
            }
            // Sinks B/C/D are metadata-only -> not class-A.
            if (ig_md.exc_b == 1) {
                ig_md.flag_b = 1;
            }
            if (ig_md.exc_c == 1) {
                ig_md.flag_c = 1;
            }
            if (ig_md.d_val == 1) {
                ig_md.flag_d = 1;
            }
        }
    }
}

control IngressDeparser(packet_out pkt, inout headers_t hdr, in ig_metadata_t ig_md,
                        in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {
    apply { pkt.emit(hdr); }
}

parser EgressParser(packet_in pkt, out headers_t hdr, out eg_metadata_t eg_md,
                    out egress_intrinsic_metadata_t eg_intr_md) {
    state start { pkt.extract(eg_intr_md); transition accept; }
}
control Egress(inout headers_t hdr, inout eg_metadata_t eg_md,
               in    egress_intrinsic_metadata_t                 eg_intr_md,
               in    egress_intrinsic_metadata_from_parser_t     eg_prsr_md,
               inout egress_intrinsic_metadata_for_deparser_t    eg_dprsr_md,
               inout egress_intrinsic_metadata_for_output_port_t eg_oport_md) {
    apply {}
}
control EgressDeparser(packet_out pkt, inout headers_t hdr, in eg_metadata_t eg_md,
                       in egress_intrinsic_metadata_for_deparser_t eg_dprsr_md) {
    apply { pkt.emit(hdr); }
}

Pipeline(IngressParser(), Ingress(), IngressDeparser(),
         EgressParser(), Egress(), EgressDeparser()) pipe;
Switch(pipe) main;
