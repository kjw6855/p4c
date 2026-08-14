/*
 * Control-plane action-parameter regression for the KEYLESS (default-action) path, TNA.
 *
 * cp_action_param_tna.p4 is the same shape with a KEYED threshold table, which reaches
 * TableStepper::evalTableControlEntries. This fixture removes the key, so the threshold action is
 * reachable only as the table's DEFAULT action and the pin has to survive
 * TableStepper::setTableDefaultEntries instead -- the SwitchV2P `switch_config` shape, and the path
 * that had no coverage at all on any target.
 *
 * Two annotation features are exercised together, because on a keyless table they are inseparable:
 *
 *   1. `default_action(tbl_get_threshold) == set_threshold`. Without it the stepper forks BOTH
 *      actions and the NoAction fork leaves ig_md.threshold at 0, so the sink flips on the first
 *      packet and the emitted test is a single-send false positive. The filter used to live in the
 *      bmv2 table stepper only; it now sits in the shared setTableDefaultEntries, which is what
 *      makes this fixture (tofino/tna) meaningful.
 *   2. `action_data(set_threshold, threshold) == 20`, enforced by cpActionArgPin on the argument
 *      symbol setTableDefaultEntries synthesizes for the default action.
 *
 * The counter is a bare `v = v + 1` RegisterAction with a header-sourced index (a constant index
 * does not anchor the chain -- see the RegisterAction data-path gap), and the sink is a TABLE KEY
 * (H2S2K) keyed on the sign bit of (est - threshold).
 *
 * Expected (tofino/tna, --path-selection STATE_DEP_TAMPERING, --cp-annotation
 * cp_action_param_keyless_tna.json):
 *   - "[CP annotation] ... installing annotated default action ... set_threshold"
 *   - "[CP annotation] ... pinning action data ... threshold = 20"
 *   - no emitted test overrides the default with NoAction
 *   - every emitted test's trace computes ig_md.threshold = 20
 *
 * Generation-only (no Tofino HW replay on this machine).
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

    // The threshold table. KEYLESS: set_threshold runs only if the controller installs it as the
    // default action, which is precisely what the `default_action` assume clause states.
    action set_threshold(bit<32> threshold) { ig_md.threshold = threshold; }
    table tbl_get_threshold {
        actions = { set_threshold; NoAction; }
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
