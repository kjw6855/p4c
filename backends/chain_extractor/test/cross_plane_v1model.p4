// v1model fixture for A2S chains of control-plane updates (see cross_plane_test.sh).
// t_cfg.set_v(v) is written as register data and read back into the key of a t_use_* table, while
// no action param reaches the read index:
//   hdr_reg:   written and read at a header index        : a2s2k to t_use_hdr
//   const_reg: written and read at a constant index      : a2s2k to t_use_const
#include <core.p4>
#include <v1model.p4>

header h_t { bit<8> idx; bit<16> k; }
struct headers_t { h_t h; }
struct meta_t { bit<16> v; bit<16> r_hdr; bit<16> r_const; bit<8> u; }

parser P(packet_in pkt, out headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    state start { pkt.extract(hdr.h); transition accept; }
}
control V(inout headers_t hdr, inout meta_t meta) { apply {} }

control I(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    register<bit<16>>(256) hdr_reg;
    register<bit<16>>(256) const_reg;

    action set_v(bit<16> v) { meta.v = v; }
    table t_cfg {
        key = { hdr.h.k : exact; }
        actions = { set_v; NoAction; }
        default_action = NoAction;
    }
    action set_u(bit<8> u) { meta.u = u; }
    table t_use_hdr {
        key = { meta.r_hdr : exact; }
        actions = { set_u; NoAction; }
        default_action = NoAction;
    }
    table t_use_const {
        key = { meta.r_const : exact; }
        actions = { set_u; NoAction; }
        default_action = NoAction;
    }

    apply {
        t_cfg.apply();
        if (hdr.h.k == 0) {
            hdr_reg.write((bit<32>)hdr.h.idx, meta.v);
            const_reg.write(0, meta.v);
        } else {
            hdr_reg.read(meta.r_hdr, (bit<32>)hdr.h.idx);
            t_use_hdr.apply();
            const_reg.read(meta.r_const, 0);
            t_use_const.apply();
        }
    }
}
control E(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) { apply {} }
control C(inout headers_t hdr, inout meta_t meta) { apply {} }
control D(packet_out pkt, in headers_t hdr) { apply { pkt.emit(hdr.h); } }
V1Switch(P(), V(), I(), E(), C(), D()) main;
