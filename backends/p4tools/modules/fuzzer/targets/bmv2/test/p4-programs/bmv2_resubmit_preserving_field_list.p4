#include <core.p4>
#include <v1model.p4>

header ethernet_t {
    bit<48> dst_addr;
    bit<48> src_addr;
    bit<16> ethertype;
}

struct headers_t {
    ethernet_t ethernet;
}

struct metadata_t {
    @field_list(0)
    bool is_recirculated;
    bool is_recirculated_without_anno;
}

parser ParserImpl(packet_in packet, out headers_t hdr, inout metadata_t meta, inout standard_metadata_t standard_metadata) {
    state start {
        packet.extract(hdr.ethernet);
        transition accept;
    }
}

control ingress(inout headers_t hdr, inout metadata_t meta, inout standard_metadata_t standard_metadata) {
    apply {
        if (!meta.is_recirculated) {
            resubmit_preserving_field_list(0);
            meta.is_recirculated = true;
            meta.is_recirculated_without_anno = true;
            hdr.ethernet.src_addr = 0xFFFFFFFFFFFF;
            return;
        }
        hdr.ethernet.ethertype = 0xAAAA;

        // This should have no effect on the ether type because we do not preserve the metadata.
        if (meta.is_recirculated_without_anno) {
            hdr.ethernet.ethertype = 0xBBBB;
        }
    }
}

control egress(inout headers_t hdr, inout metadata_t meta, inout standard_metadata_t standard_metadata) {
    apply {
    }
}

control DeparserImpl(packet_out packet, in headers_t hdr) {
    apply {
        packet.emit(hdr.ethernet);
    }
}


control verifyChecksum(inout headers_t hdr, inout metadata_t meta) {
    apply {}
}

control computeChecksum(inout headers_t hdr, inout metadata_t meta) {
    apply {}
}

V1Switch(ParserImpl(), verifyChecksum(), ingress(), egress(), computeChecksum(), DeparserImpl()) main;

