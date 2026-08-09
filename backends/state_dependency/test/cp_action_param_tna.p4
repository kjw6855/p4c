/*
 * Control-plane action-parameter threshold regression fixture (SketchLib shape), TNA.
 *
 * Distinct from accum_threshold_tna.p4 in the two ways that matter:
 *
 *   1. The RegisterAction holds NO threshold. It is a bare `v = v + 1`, so there is no
 *      IR::Operation_Relation with a constant operand anywhere in the chain's writeNodes and the
 *      analytical drive-register has nothing in the program text to solve a packet count against.
 *      The threshold instead arrives as a CONTROL-PLANE ACTION PARAMETER
 *      (`set_threshold(bit<32> threshold)`) and is compared OUTSIDE the RegisterAction, exactly as
 *      SketchLib's countmin does via `tbl_get_threshold_act` / `est = est - threshold`.
 *
 *   2. The sink is a TABLE KEY (H2S2K), not an if-condition. accum_threshold_tna.p4 covers the
 *      condition path, which already had the driver; this covers the key path.
 *
 * Without the annotation the synthesized `threshold` argument is free-symbolic, so the solver is
 * free to pick 0, the sink flips on the first packet, and the emitted test is a single-send false
 * positive. With `cp_action_param_tna.json` pinning threshold = 20, the argument is constrained at
 * synthesis time and the driver derives k ~= 20 (the counter must reach the threshold), emitted as
 * `repeat_count`.
 *
 * The index is header-sourced so the RegisterAction chain anchors (a constant index does not --
 * see the RegisterAction data-path gap).
 *
 * Expected (tofino/tna, --path-selection STATE_DEP_TAMPERING, --cp-annotation cp_action_param_tna.json):
 *   - the pin fires ("[CP annotation] ... pinning action data ... = 20")
 *   - at least one emitted txtpb carries repeat_count of 20 or 21 (at- vs strictly-above threshold)
 *   - the emitted entry's threshold really is 20, so k and the entry agree
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
    bit<32> threshold;  // supplied by the control plane, never by the packet
    bit<32> est;        // counter value read back out of the SALU
    bit<1>  above;      // sign bit of (est - threshold): 0 once est >= threshold
}
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        ig_md.threshold = 0;
        ig_md.est = 0;
        ig_md.above = 0;
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

    // Plain increment-only accumulator: no threshold, no relation, nothing to harvest.
    Register<bit<32>, bit<8>>(256, 0) ctr;
    RegisterAction<bit<32>, bit<8>, bit<32>>(ctr) bump = {
        void apply(inout bit<32> v, out bit<32> est) {
            v = v + 1;
            est = v;
        }
    };

    // The threshold table. Its action parameter is what an `assume` clause pins.
    action set_threshold(bit<32> threshold) { ig_md.threshold = threshold; }
    table tbl_get_threshold {
        key = { hdr.ethernet.ether_type : exact; }
        actions = { set_threshold; NoAction; }
        default_action = NoAction();
        size = 4;
    }

    // H2S2K sink: keyed on the accumulated-vs-threshold comparison.
    action fwd() { ig_tm_md.ucast_egress_port = 1; }
    action drop() { ig_dprsr_md.drop_ctl = 0x1; }
    table sink_tbl {
        key = { ig_md.above : exact; }
        actions = { fwd; drop; }
        default_action = drop();
        size = 4;
    }

    apply {
        if (hdr.ipv4.isValid()) {
            tbl_get_threshold.apply();
            bit<8> idx = hdr.ipv4.diffserv;   // header-sourced index (anchors the chain)
            ig_md.est = bump.execute(idx);
            // Compared OUTSIDE the RegisterAction, against a control-plane value. Unsigned wrap
            // makes the MSB the "below threshold" flag: est < threshold => borrow => MSB set.
            ig_md.est = ig_md.est - ig_md.threshold;
            ig_md.above = (bit<1>)((ig_md.est >> 31) & 32w1);
            sink_tbl.apply();
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
