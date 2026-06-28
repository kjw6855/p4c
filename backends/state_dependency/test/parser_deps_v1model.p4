/*
 * Regression fixture for --parser-deps (parser-state dependency record).
 *
 * The parser derives BOTH the register index and the written value from headers
 * (meta.idx = hdr.ipv4.dstAddr; meta.data = hdr.ipv4.identification). The ingress control then uses
 * ONLY metadata (no direct header refs): it writes/reads reg[meta.idx] and conditions on the value.
 *
 * Single-control analysis sees meta.idx/meta.data as unsourced -> NO chain. With --parser-deps the
 * parser-state record seeds them as sources, so the SO->condition chain is found and rooted at the
 * parser-derived metadata (with per-chain header pins meta.idx<-hdr.dstAddr, meta.data<-hdr.id).
 * parser_deps_test.sh asserts exactly that difference.
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
struct metadata { bit<32> idx; bit<16> data; bit<16> val; }

parser MyParser(packet_in packet, out headers hdr, inout metadata meta,
                inout standard_metadata_t std) {
    state start {
        packet.extract(hdr.ethernet);
        packet.extract(hdr.ipv4);
        meta.idx = hdr.ipv4.dstAddr;          // header -> metadata (index)
        meta.data = hdr.ipv4.identification;  // header -> metadata (value)
        transition accept;
    }
}

control MyVerify(inout headers hdr, inout metadata meta) { apply { } }

control MyIngress(inout headers hdr, inout metadata meta, inout standard_metadata_t std) {
    register<bit<16>>(64) reg;
    apply {
        reg.write(meta.idx, meta.data);   // no direct header refs in the control
        reg.read(meta.val, meta.idx);
        if (meta.val == 16w5) {
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
