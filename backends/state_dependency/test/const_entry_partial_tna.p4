/*
 * Const-entry PARTIAL coverage fixture (negative control) (linkguardian decide_retx_or_drop shape), TNA.
 *
 * Same shape as const_entry_sink_tna.p4 but the `const entries` cover only PART of the key space:
 * (1,0) is named by no entry, so the table CAN still MISS and a HIT<->MISS tamper against it remains
 * a legitimate finding. This is the over-suppression guard -- the coverage check must answer "not
 * covered" here, not "has const entries, therefore covered".
 *
 * Ternary keys are kept so the only difference from the positive fixture is which points are named.
 *
 * Expected: the suppression must NOT fire, and generation proceeds as before this change.
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
struct ig_metadata_t { bit<1> from_reg; bit<1> from_hdr; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        ig_md.from_reg = 0;
        ig_md.from_hdr = 0;
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

    Register<bit<32>, bit<8>>(256, 0) flag_reg;
    RegisterAction<bit<32>, bit<8>, bit<1>>(flag_reg) read_flag = {
        void apply(inout bit<32> v, out bit<1> rv) {
            rv = (bit<1>)(v & 32w1);
            v = v + 1;
        }
    };

    action fwd() { ig_tm_md.ucast_egress_port = 1; }
    action drop() { ig_dprsr_md.drop_ctl = 0x1; }

    table sink_tbl {
        key = {
            ig_md.from_reg : ternary;
            ig_md.from_hdr : ternary;
        }
        actions = { fwd; drop; }
        default_action = drop();
        size = 4;
        // Deliberately INCOMPLETE: (1,0) is named by no entry, so the table can still MISS and
        // the suppression must NOT fire. Guards against a coverage check that answers "covered" for
        // any table that merely has const entries.
        const entries = {
            (0, 0) : drop();
            (0, 1) : fwd();
            (1, 1) : fwd();
        }
    }

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;         // header-sourced index (anchors the chain)
            ig_md.from_reg = read_flag.execute(idx);
            ig_md.from_hdr = (bit<1>)(hdr.ipv4.ttl & 8w1);
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
