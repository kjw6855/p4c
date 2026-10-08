// tna fixture for the output relations of p4c_chain_extractor (see output_sinks_test.sh).
// Registers are written and read at a header-chosen index, so each read starts a header -> SO chain:
//   port_reg  -> ig_tm_md.ucast_egress_port       : H2S2O    (data flow into the output)
//   drop_reg  -> if (..) ig_dprsr_md.drop_ctl = 1  : H2S2OutC (condition guards a drop)
//   decoy_reg -> if (..) ig_md.unused = 1          : H2S2C only, not an output relation
//   k2_reg    -> key of t_direct (action fwd sets ucast_egress_port)
//                                                  : H2S2OutK
//   k1_reg    -> key of t_mid (sets ig_md.nh) -> key of t_out (sets drop_ctl)
//                                                  : H2S2OutK through an intermediate table
//   kdec_reg  -> key of t_decoy (sets ig_md.unused): H2S2K only, not an output relation
#include <core.p4>
#include <tna.p4>

header ethernet_h { bit<48> dst_addr; bit<48> src_addr; bit<16> ether_type; }
header ctrl_h { bit<8> idx; bit<16> val; }

struct headers_t { ethernet_h ethernet; ctrl_h ctrl; }
struct ig_metadata_t { bit<8> unused; bit<16> k1; bit<8> nh; bit<16> k2; bit<16> kdec; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        pkt.extract(hdr.ethernet);
        pkt.extract(hdr.ctrl);
        transition accept;
    }
}

control Ingress(inout headers_t hdr, inout ig_metadata_t ig_md,
                in    ingress_intrinsic_metadata_t              ig_intr_md,
                in    ingress_intrinsic_metadata_from_parser_t  ig_prsr_md,
                inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                inout ingress_intrinsic_metadata_for_tm_t       ig_tm_md) {

    Register<bit<16>, bit<8>>(256, 0) port_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(port_reg) set_port = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(port_reg) get_port = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) drop_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(drop_reg) set_drop = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(drop_reg) get_drop = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) decoy_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(decoy_reg) set_decoy = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(decoy_reg) get_decoy = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) k1_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(k1_reg) set_k1_reg = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(k1_reg) get_k1_reg = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) k2_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(k2_reg) set_k2_reg = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(k2_reg) get_k2_reg = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) kdec_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(kdec_reg) set_kdec_reg = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(kdec_reg) get_kdec_reg = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    action set_nh(bit<8> nh) { ig_md.nh = nh; }
    table t_mid {
        key = { ig_md.k1 : exact; }
        actions = { set_nh; NoAction; }
        default_action = NoAction;
    }
    action do_drop() { ig_dprsr_md.drop_ctl = 1; }
    table t_out {
        key = { ig_md.nh : exact; }
        actions = { do_drop; NoAction; }
        default_action = NoAction;
    }
    action fwd(PortId_t port) { ig_tm_md.ucast_egress_port = port; }
    table t_direct {
        key = { ig_md.k2 : exact; }
        actions = { fwd; NoAction; }
        default_action = NoAction;
    }
    action set_unused(bit<8> u) { ig_md.unused = u; }
    table t_decoy {
        key = { ig_md.kdec : exact; }
        actions = { set_unused; NoAction; }
        default_action = NoAction;
    }

    apply {
        bit<8> idx = hdr.ctrl.idx;
        if (hdr.ethernet.ether_type == 0x1234) {
            set_port.execute(idx);
            set_drop.execute(idx);
            set_decoy.execute(idx);
            set_k1_reg.execute(idx);
            set_k2_reg.execute(idx);
            set_kdec_reg.execute(idx);
        } else {
            bit<16> port = get_port.execute(idx);
            ig_tm_md.ucast_egress_port = (PortId_t) port;

            bit<16> dropFlag = get_drop.execute(idx);
            if (dropFlag == 1) {
                ig_dprsr_md.drop_ctl = 1;
            }

            bit<16> decoy = get_decoy.execute(idx);
            if (decoy == 3) {
                ig_md.unused = 1;
            }

            ig_md.k1 = get_k1_reg.execute(idx);
            t_mid.apply();
            t_out.apply();
            ig_md.k2 = get_k2_reg.execute(idx);
            t_direct.apply();
            ig_md.kdec = get_kdec_reg.execute(idx);
            t_decoy.apply();
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
