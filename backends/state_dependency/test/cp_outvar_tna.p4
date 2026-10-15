/*
 * --state-dep-sinks O fixture (TNA), used with cp_outvar_tna.txtpb. A register value is written
 * straight into an output variable (H2S2O); the attacker (protocol 17) overwrites the cell.
 *   reg_p -> ucast_egress_port   (ttl 1): the controller set reg_p[5] = 2
 *   reg_m -> mcast_grp_b         (ttl 2): the controller set reg_m[5] = 1; groups 1 and 2 both
 *                                         replicate to ports {1,2}, group 3 to {3}; the
 *                                         packet also carries mcast_grp_a = 128, a group the
 *                                         snapshot never creates, so it adds no replica port
 * With --state-tamper-value 3 both chains change where the victim's packet goes; with 2 neither
 * does (port 2 stays 2, group 2 = group 1's ports) -- see cp_outsinks_test.sh.
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
struct ig_metadata_t { bit<16> p; bit<16> m; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        ig_md.p = 0;
        ig_md.m = 0;
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

    Register<bit<16>, bit<8>>(256, 0) reg_p;
    Register<bit<16>, bit<8>>(256, 0) reg_m;
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_p) read_p = {
        void apply(inout bit<16> v, out bit<16> rv) { rv = v; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_p) write_p = {
        void apply(inout bit<16> v) { v = hdr.ipv4.identification; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_m) read_m = {
        void apply(inout bit<16> v, out bit<16> rv) { rv = v; } };
    RegisterAction<bit<16>, bit<8>, bit<16>>(reg_m) write_m = {
        void apply(inout bit<16> v) { v = hdr.ipv4.identification; } };

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;
            if (hdr.ipv4.protocol == 17) {
                if (hdr.ipv4.ttl == 1) { write_p.execute(idx); }
                else { write_m.execute(idx); }
                ig_dprsr_md.drop_ctl = 0x1;
            } else if (hdr.ipv4.ttl == 1) {
                ig_md.p = read_p.execute(idx);
                ig_tm_md.ucast_egress_port = (PortId_t)ig_md.p;
            } else if (hdr.ipv4.ttl == 2) {
                ig_md.m = read_m.execute(idx);
                ig_tm_md.mcast_grp_a = 16w128;
                ig_tm_md.mcast_grp_b = ig_md.m;
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
