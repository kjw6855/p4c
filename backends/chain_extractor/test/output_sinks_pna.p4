// pna fixture for the output relations of p4c_chain_extractor (see output_sinks_test.sh).
// PNA decides the output through externs (send_to_port, drop_packet), not metadata fields.
// The stateful objects are add-on-miss tables, as in k8s_dpdk: each miss stores a header field
// through add_entry, and a later hit reads it back as the hit action's param, so each hit read
// starts a header -> SO chain:
//   flows_d  -> if (..) drop_packet()              : H2S2OutC (condition guards a drop)
//   flows_c  -> if (..) meta.unused = 1            : H2S2C only, not an output relation
//   flows_k  -> key of t_fwd (action fwd calls send_to_port)
//                                                  : H2S2OutK
//   flows_kd -> key of t_decoy (sets meta.unused)  : H2S2K only, not an output relation
#include <core.p4>
#include <pna.p4>

header h_t {
    bit<32> src;
    bit<32> a;
    bit<32> b;
    bit<32> c;
    bit<32> d;
}

struct headers_t {
    h_t h;
}

struct main_metadata_t {
    bit<32> d;
    bit<32> c;
    bit<32> k;
    bit<32> kdec;
    bit<8>  unused;
}

control PreControlImpl(in headers_t hdr, inout main_metadata_t meta,
                       in pna_pre_input_metadata_t istd, inout pna_pre_output_metadata_t ostd) {
    apply { }
}

parser MainParserImpl(packet_in pkt, out headers_t hdr, inout main_metadata_t meta,
                      in pna_main_parser_input_metadata_t istd) {
    state start {
        pkt.extract(hdr.h);
        transition accept;
    }
}

control MainControlImpl(inout headers_t hdr, inout main_metadata_t meta,
                        in pna_main_input_metadata_t istd, inout pna_main_output_metadata_t ostd) {
    action d_hit(bit<32> v) { meta.d = v; }
    action d_miss() {
        add_entry(action_name = "d_hit", action_params = {hdr.h.a},
                  expire_time_profile_id = (ExpireTimeProfileId_t)0);
    }
    table flows_d {
        key = { hdr.h.src : exact; }
        actions = { @tableonly d_hit; @defaultonly d_miss; }
        add_on_miss = true;
        const default_action = d_miss;
    }

    action c_hit(bit<32> v) { meta.c = v; }
    action c_miss() {
        add_entry(action_name = "c_hit", action_params = {hdr.h.b},
                  expire_time_profile_id = (ExpireTimeProfileId_t)0);
    }
    table flows_c {
        key = { hdr.h.src : exact; }
        actions = { @tableonly c_hit; @defaultonly c_miss; }
        add_on_miss = true;
        const default_action = c_miss;
    }

    action k_hit(bit<32> v) { meta.k = v; }
    action k_miss() {
        add_entry(action_name = "k_hit", action_params = {hdr.h.c},
                  expire_time_profile_id = (ExpireTimeProfileId_t)0);
    }
    table flows_k {
        key = { hdr.h.src : exact; }
        actions = { @tableonly k_hit; @defaultonly k_miss; }
        add_on_miss = true;
        const default_action = k_miss;
    }

    action kd_hit(bit<32> v) { meta.kdec = v; }
    action kd_miss() {
        add_entry(action_name = "kd_hit", action_params = {hdr.h.d},
                  expire_time_profile_id = (ExpireTimeProfileId_t)0);
    }
    table flows_kd {
        key = { hdr.h.src : exact; }
        actions = { @tableonly kd_hit; @defaultonly kd_miss; }
        add_on_miss = true;
        const default_action = kd_miss;
    }

    action fwd(PortId_t port) {
        send_to_port(port);
    }
    table t_fwd {
        key = { meta.k : exact; }
        actions = { fwd; NoAction; }
        default_action = NoAction();
    }

    action mark(bit<8> v) {
        meta.unused = v;
    }
    table t_decoy {
        key = { meta.kdec : exact; }
        actions = { mark; NoAction; }
        default_action = NoAction();
    }

    apply {
        flows_d.apply();
        if (meta.d == 1) {
            drop_packet();
        }

        flows_c.apply();
        if (meta.c == 1) {
            meta.unused = 1;
        }

        flows_k.apply();
        t_fwd.apply();

        flows_kd.apply();
        t_decoy.apply();
    }
}

control MainDeparserImpl(packet_out pkt, in headers_t hdr, in main_metadata_t meta,
                         in pna_main_output_metadata_t ostd) {
    apply {
        pkt.emit(hdr.h);
    }
}

PNA_NIC(MainParserImpl(), PreControlImpl(), MainControlImpl(), MainDeparserImpl()) main;
