// Fixture for add_on_miss_test.sh: an add-on-miss table as a stateful object.
//
// t_ptr's action param reaches add_entry's data (write); a later hit of `flows` reads it back as
// flows_hit's param (read), which keys t_use. The cache must list this under a2s2k with soName
// MainControlImpl.flows and sink table t_use.
#include <core.p4>
#include <pna.p4>

header ipv4_t {
    bit<8>  protocol;
    bit<32> srcAddr;
    bit<32> dstAddr;
}

struct headers_t {
    ipv4_t ipv4;
}

struct main_metadata_t {
    bit<24> ptr;
    bit<24> hit_ptr;
}

control PreControlImpl(in headers_t hdr, inout main_metadata_t meta,
                       in pna_pre_input_metadata_t istd, inout pna_pre_output_metadata_t ostd) {
    apply { }
}

parser MainParserImpl(packet_in pkt, out headers_t hdr, inout main_metadata_t meta,
                      in pna_main_parser_input_metadata_t istd) {
    state start {
        pkt.extract(hdr.ipv4);
        transition accept;
    }
}

control MainControlImpl(inout headers_t hdr, inout main_metadata_t meta,
                        in pna_main_input_metadata_t istd, inout pna_main_output_metadata_t ostd) {
    action set_ptr(bit<24> ptr) {
        meta.ptr = ptr;
    }
    table t_ptr {
        key = { hdr.ipv4.protocol : exact; }
        actions = { set_ptr; }
    }

    action flows_hit(bit<24> ptr) {
        meta.hit_ptr = ptr;
    }
    action flows_miss() {
        add_entry(action_name = "flows_hit", action_params = {meta.ptr},
                  expire_time_profile_id = (ExpireTimeProfileId_t)0);
    }
    table flows {
        key = {
            hdr.ipv4.srcAddr : exact;
            hdr.ipv4.dstAddr : exact;
        }
        actions = {
            @tableonly flows_hit;
            @defaultonly flows_miss;
        }
        add_on_miss = true;
        const default_action = flows_miss;
    }

    action fwd(PortId_t port) {
        send_to_port(port);
    }
    table t_use {
        key = { meta.hit_ptr : exact; }
        actions = { fwd; }
    }

    apply {
        t_ptr.apply();
        flows.apply();
        t_use.apply();
    }
}

control MainDeparserImpl(packet_out pkt, in headers_t hdr, in main_metadata_t meta,
                         in pna_main_output_metadata_t ostd) {
    apply {
        pkt.emit(hdr.ipv4);
    }
}

PNA_NIC(MainParserImpl(), PreControlImpl(), MainControlImpl(), MainDeparserImpl()) main;
