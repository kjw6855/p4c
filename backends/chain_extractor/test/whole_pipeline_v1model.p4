/*
 * De-risk / regression fixture for whole-pipeline state-dependency analysis.
 *
 * The parser writes a header field into metadata (meta.data = hdr.ipv4.identification). The ingress
 * then writes that metadata into a register, reads it back, and gates a condition on the value
 * (an H2S2C: stateful-object value reaches an if-condition).
 *
 * Per-control analysis sees meta.data as an unsourced root, so the chain has NO header provenance.
 * Whole-pipeline analysis (--whole-pipeline) links the parser's meta.data def to the ingress use by
 * source-position/.equiv() canonicalization, so the chain is rooted at hdr.ipv4.identification.
 * whole_pipeline_test.sh asserts exactly that difference.
 */
#include <core.p4>
#include <v1model.p4>

header ethernet_t { bit<48> dst; bit<48> src; bit<16> etype; }
header ipv4_t {
    bit<8>  ver_ihl; bit<8> diffserv; bit<16> totalLen; bit<16> identification;
    bit<16> flags_frag; bit<8> ttl; bit<8> proto; bit<16> hdrChecksum;
    bit<32> srcAddr; bit<32> dstAddr;
}
struct headers { ethernet_t ethernet; ipv4_t ipv4; }
struct metadata { bit<16> data; bit<16> val; }

parser MyParser(packet_in packet, out headers hdr, inout metadata meta,
                inout standard_metadata_t std) {
    state start {
        packet.extract(hdr.ethernet);
        packet.extract(hdr.ipv4);
        meta.data = hdr.ipv4.identification;   // header -> metadata (cross-block link, in the PARSER)
        transition accept;
    }
}

control MyVerify(inout headers hdr, inout metadata meta) { apply { } }

control MyIngress(inout headers hdr, inout metadata meta, inout standard_metadata_t std) {
    register<bit<16>>(64) reg;
    apply {
        reg.write(hdr.ipv4.dstAddr, meta.data);   // header-derived (via parser) metadata -> SO data
        reg.read(meta.val, hdr.ipv4.dstAddr);     // SO value -> metadata
        if (meta.val == 16w5) {                   // SO value gates a condition (H2S2C sink)
            std.egress_spec = 1;
        } else {
            mark_to_drop(std);
        }
    }
}

control MyEgress(inout headers hdr, inout metadata meta, inout standard_metadata_t std) { apply { } }
control MyCompute(inout headers hdr, inout metadata meta) { apply { } }
control MyDeparser(packet_out packet, in headers hdr) {
    apply { packet.emit(hdr.ethernet); packet.emit(hdr.ipv4); }
}

V1Switch(MyParser(), MyVerify(), MyIngress(), MyEgress(), MyCompute(), MyDeparser()) main;
