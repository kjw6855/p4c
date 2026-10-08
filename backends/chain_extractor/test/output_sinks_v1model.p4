// Fixture for the output relations of p4c_chain_extractor (see output_sinks_test.sh).
// Every register is written and read at a header-chosen index, so each read starts a
// header -> SO chain:
//   r_port  -> egress_spec                      : H2S2O    (data flow into the output)
//   r_drop  -> if (..) mark_to_drop             : H2S2OutC (condition guards a drop)
//   r_nh    -> key of t_nh -> meta.nh -> if (..) egress_spec = 2
//                                               : H2S2OutC through an intermediate table
//   r_decoy -> if (..) meta.unused = 1          : H2S2C only, not an output relation
//   r_cfg   -> mcast_grp, indexed by the action param of t_cfg
//                                               : A2S2O (action param -> SO -> output)
// Nothing overwrites a guarded output write afterwards: a later write to the same variable
// currently also drops the chain to the guarding condition (in p4c_state_dependency as well).
#include <core.p4>
#include <v1model.p4>

header h_t {
    bit<32> a;
    bit<32> b;
    bit<32> c;
    bit<32> d;
}

struct headers_t {
    h_t h;
}

struct meta_t {
    bit<32> k;
    bit<8> nh;
    bit<8> unused;
    bit<32> idx;
}

parser P(packet_in pkt, out headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    state start {
        pkt.extract(hdr.h);
        transition accept;
    }
}

control V(inout headers_t hdr, inout meta_t meta) { apply { } }

control I(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    register<bit<32>>(16) r_port;
    register<bit<32>>(16) r_drop;
    register<bit<32>>(16) r_nh;
    register<bit<32>>(16) r_decoy;
    register<bit<32>>(16) r_cfg;

    action set_idx(bit<32> i) { meta.idx = i; }
    table t_cfg {
        key = { hdr.h.a: exact; }
        actions = { set_idx; NoAction; }
        default_action = NoAction();
    }

    action set_nh(bit<8> nh) { meta.nh = nh; }
    action nh_decoy(bit<32> v) { meta.idx = v; }   // never reaches the guard: not selected
    table t_nh {
        key = { meta.k: exact; }
        actions = { set_nh; nh_decoy; NoAction; }
        default_action = NoAction();
    }

    apply {
        r_port.write(hdr.h.a, hdr.h.a);
        r_drop.write(hdr.h.b, hdr.h.b);
        r_nh.write(hdr.h.c, hdr.h.c);
        r_decoy.write(hdr.h.d, hdr.h.d);

        bit<32> port;
        r_port.read(port, hdr.h.a);
        sm.egress_spec = (bit<9>) port;

        bit<32> dropFlag;
        r_drop.read(dropFlag, hdr.h.b);
        if (dropFlag == 1) {
            mark_to_drop(sm);
        }

        bit<32> k;
        r_nh.read(k, hdr.h.c);
        meta.k = k;
        t_nh.apply();
        if (meta.nh == 7) {
            sm.egress_spec = 2;
        }

        bit<32> decoy;
        r_decoy.read(decoy, hdr.h.d);
        if (decoy == 3) {
            meta.unused = 1;
        }

        t_cfg.apply();
        r_cfg.write(meta.idx, hdr.h.b);
        bit<32> cfgPort;
        r_cfg.read(cfgPort, meta.idx);
        sm.mcast_grp = (bit<16>) cfgPort;
    }
}

control E(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) { apply { } }
control C(inout headers_t hdr, inout meta_t meta) { apply { } }
control D(packet_out pkt, in headers_t hdr) { apply { pkt.emit(hdr.h); } }

V1Switch(P(), V(), I(), E(), C(), D()) main;
