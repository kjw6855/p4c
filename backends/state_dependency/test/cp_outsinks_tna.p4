/*
 * --state-dep-sinks OUTK and --diverge-output fixture (TNA), used with cp_outsinks_tna.txtpb. Two
 * registers, each written by an attacker packet (protocol 17) and read by a victim at cell
 * diffserv; the controller wrote 0x0800 to cell 5 of both.
 *   reg_a -> sink_a (OutK): HIT fwd(2), MISS drop          -> the flip changes the PORT
 *   reg_b -> sink_b (OutK): HIT fwd_mark(2) (port 2, ttl rewritten), MISS fwd(2)
 *                                                           -> same port, different BYTES
 * --diverge-output PORT must find sink_a but not sink_b; PORT_AND_BYTES finds both.
 * (OUTC is covered by cp_outc_tna.p4.)
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
struct ig_metadata_t { bit<16> a; bit<16> b; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        ig_md.a = 0;
        ig_md.b = 0;
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

    Register<bit<16>, bit<8>>(256, 0) reg_a;
    Register<bit<16>, bit<8>>(256, 0) reg_b;
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_a) read_a = {
        void apply(inout bit<16> v, out bit<16> rv) { rv = v; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_a) write_a = {
        void apply(inout bit<16> v) { v = hdr.ipv4.identification; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_b) read_b = {
        void apply(inout bit<16> v, out bit<16> rv) { rv = v; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_b) write_b = {
        void apply(inout bit<16> v) { v = hdr.ipv4.identification; } };

    action fwd(PortId_t port) { ig_tm_md.ucast_egress_port = port; }
    action fwd_mark(PortId_t port) { ig_tm_md.ucast_egress_port = port; hdr.ipv4.ttl = 1; }
    action drop() { ig_dprsr_md.drop_ctl = 0x1; }
    table sink_a {
        key = { ig_md.a : exact; }
        actions = { fwd; drop; }
        default_action = drop();
        size = 16;
    }
    table sink_b {
        key = { ig_md.b : exact; }
        actions = { fwd; fwd_mark; }
        default_action = fwd(2);
        size = 16;
    }

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;
            if (hdr.ipv4.protocol == 17) {
                if (hdr.ipv4.ttl == 1) { write_a.execute(idx); }
                else { write_b.execute(idx); }
                drop();
            } else if (hdr.ipv4.ttl == 1) {
                ig_md.a = read_a.execute(idx);
                sink_a.apply();
            } else if (hdr.ipv4.ttl == 2) {
                ig_md.b = read_b.execute(idx);
                sink_b.apply();
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
