#include <core.p4>
#define V1MODEL_VERSION 20180101
#include <v1model.p4>

header ethernet_t {
    bit<48> dst_addr;
    bit<48> src_addr;
    bit<16> eth_type;
}

struct Headers {
    ethernet_t eth_hdr;
}

struct Meta {
}

parser p(packet_in pkt, out Headers hdr, inout Meta m, inout standard_metadata_t sm) {
    state start {
        pkt.extract<ethernet_t>(hdr.eth_hdr);
        transition accept;
    }
}

control ingress(inout Headers h, inout Meta m, inout standard_metadata_t sm) {
    @name("ingress.tmp_val") bit<48> tmp_val_0;
    @name("ingress.tmp") bit<48> tmp;
    @name("ingress.simple_action") action simple_action() {
        tmp = (h.eth_hdr.eth_type != 16w0 ? (h.eth_hdr.src_addr != 48w0 ? 48w1 : 48w2) : tmp);
        tmp_val_0 = (h.eth_hdr.eth_type != 16w0 ? tmp : tmp_val_0);
        h.eth_hdr.dst_addr = (h.eth_hdr.eth_type != 16w0 ? tmp_val_0 : h.eth_hdr.dst_addr);
    }
    @hidden table tbl_simple_action {
        actions = {
            simple_action();
        }
        const default_action = simple_action();
    }
    apply {
        tbl_simple_action.apply();
    }
}

control vrfy(inout Headers h, inout Meta m) {
    apply {
    }
}

control update(inout Headers h, inout Meta m) {
    apply {
    }
}

control egress(inout Headers h, inout Meta m, inout standard_metadata_t sm) {
    apply {
    }
}

control deparser(packet_out b, in Headers h) {
    apply {
        b.emit<ethernet_t>(h.eth_hdr);
    }
}

V1Switch<Headers, Meta>(p(), vrfy(), ingress(), egress(), update(), deparser()) main;
