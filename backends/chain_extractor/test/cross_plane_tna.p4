// tna fixture for A2S chains of control-plane updates (see cross_plane_test.sh).
//   inc_reg: t_cfg.set_v(v) reaches a RegisterAction body (`v = ig_md.v`) written at a header
//            index, then read at a header index into the key of t_use_inc
//                                                           : inconsistency (writeData = set_v.v)
//   rad_reg: the same body write, read at the set_slot index into the key of t_use_rad
//                                                           : inconsistency through a body write
//   res_reg: t_slot.set_slot(s) indexes both the write (header data) and the read, which keys
//            t_use_res                                      : residual (writeIdx = readIdx = set_slot.s)
//   rmw_reg: a read-modify-write at the set_slot index, its result keys t_use_rmw
//                                                           : residual on an isUpdate chain
#include <core.p4>
#include <tna.p4>

header ethernet_h { bit<48> dst_addr; bit<48> src_addr; bit<16> ether_type; }
header ctrl_h { bit<8> idx; bit<16> val; }

struct headers_t { ethernet_h ethernet; ctrl_h ctrl; }
struct ig_metadata_t {
    bit<16> v; bit<8> slot; bit<16> r_inc; bit<16> r_rad; bit<16> r_res; bit<16> r_rmw;
    bit<8> unused;
}
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

    Register<bit<16>, bit<8>>(256, 0) inc_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(inc_reg) set_inc = {
        void apply(inout bit<16> v) { v = ig_md.v; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(inc_reg) get_inc = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) rad_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(rad_reg) set_rad = {
        void apply(inout bit<16> v) { v = ig_md.v; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(rad_reg) get_rad = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) res_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(res_reg) set_res = {
        void apply(inout bit<16> v) { v = hdr.ctrl.val; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(res_reg) get_res = {
        void apply(inout bit<16> v, out bit<16> r) { r = v; }
    };

    Register<bit<16>, bit<8>>(256, 0) rmw_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(rmw_reg) bump_rmw = {
        void apply(inout bit<16> v, out bit<16> r) { v = v + 1; r = v; }
    };

    action set_v(bit<16> v) { ig_md.v = v; }
    table t_cfg {
        key = { hdr.ethernet.ether_type : exact; }
        actions = { set_v; NoAction; }
        default_action = NoAction;
    }
    action set_slot(bit<8> s) { ig_md.slot = s; }
    table t_slot {
        key = { hdr.ethernet.src_addr : exact; }
        actions = { set_slot; NoAction; }
        default_action = NoAction;
    }

    action set_unused(bit<8> u) { ig_md.unused = u; }
    table t_use_inc {
        key = { ig_md.r_inc : exact; }
        actions = { set_unused; NoAction; }
        default_action = NoAction;
    }
    table t_use_rad {
        key = { ig_md.r_rad : exact; }
        actions = { set_unused; NoAction; }
        default_action = NoAction;
    }
    table t_use_res {
        key = { ig_md.r_res : exact; }
        actions = { set_unused; NoAction; }
        default_action = NoAction;
    }
    table t_use_rmw {
        key = { ig_md.r_rmw : exact; }
        actions = { set_unused; NoAction; }
        default_action = NoAction;
    }

    apply {
        t_cfg.apply();
        t_slot.apply();
        if (hdr.ethernet.dst_addr == 0) {
            set_inc.execute(hdr.ctrl.idx);
            set_rad.execute(hdr.ctrl.idx);
            set_res.execute(ig_md.slot);
        } else {
            ig_md.r_inc = get_inc.execute(hdr.ctrl.idx);
            t_use_inc.apply();
            ig_md.r_rad = get_rad.execute(ig_md.slot);
            t_use_rad.apply();
            ig_md.r_res = get_res.execute(ig_md.slot);
            t_use_res.apply();
        }
        ig_md.r_rmw = bump_rmw.execute(ig_md.slot);
        t_use_rmw.apply();
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
