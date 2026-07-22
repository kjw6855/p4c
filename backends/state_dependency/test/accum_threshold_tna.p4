/*
 * Phase-1 accumulation regression fixture (ACC-Turbo counting-bloom shape), TNA.
 *
 * A counter register is incremented once per packet inside a RegisterAction and gated by a constant
 * threshold C=20 (small enough that the DFS terminates; C>15 so the analytically-solved k>16, which
 * the emitter writes as a single `repeat_count` block rather than k literal `tamper_only` blocks —
 * see the bfrt emitter split). The threshold-exceeded flag feeds an if-condition sink (H2S2C). A
 * single Phase-2 write cannot cross the threshold, so the tamper is only realizable by ACCUMULATION:
 * replay the Phase-2 packet k times (k ~= C+1) until `v > C` flips `exceeded` and the sink diverges.
 *
 * This exercises the analytical drive-register path (driveRegisterPhase2): it measures (base,delta),
 * solves k = ceil((C+1 - base)/delta) + 1, and emits repeat_count = k. The index is header-sourced
 * so the RegisterAction chain anchors (a constant index does not — see RegisterAction data-path gap).
 *
 * Expected (tofino/tna, --path-selection STATE_DEP_TAMPERING_COND):
 *   at least one emitted txtpb has repeat_count > 1 (single-packet emission would be a false positive
 *   that cannot cross the threshold on hardware).
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
struct ig_metadata_t { bit<1> exceeded; }
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

    // Per-flow counter (header-indexed so the chain anchors), increment-only RMW with a threshold.
    Register<bit<32>, bit<8>>(256, 0) ctr;
    RegisterAction<bit<32>, bit<8>, bit<1>>(ctr) bump = {
        void apply(inout bit<32> v, out bit<1> exceeded) {
            exceeded = 0;
            if (v > 20) { exceeded = 1; }  // threshold C=20 (k>16 -> emitted as repeat_count)
            v = v + 1;                     // increment (delta = 1)
        }
    };

    action drop() { ig_dprsr_md.drop_ctl = 0x1; }
    table drop_tbl {
        actions = { drop; NoAction; }
        default_action = drop();
        size = 1;
    }

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;          // header-sourced index (anchors the chain)
            ig_md.exceeded = bump.execute(idx);
            if (ig_md.exceeded == 1) {               // H2S2C sink: gated by the accumulated counter
                drop_tbl.apply();
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
