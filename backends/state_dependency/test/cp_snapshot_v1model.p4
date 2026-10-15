/*
 * --cp-snapshot fixture (v1model), the bmv2 counterpart of cp_snapshot_tna.p4: the keyless cfg's
 * runtime default opens the stateful path, sink_tbl forwards only the installed key, and
 * state_reg[5] holds the key because the controller wrote it. The snapshot is P4Runtime entities
 * named through the name fields; its register entry is addressed by P4Info id, which the test fills
 * in from p4c-bm2-ss's P4Info. opt_tbl checks optional keys: a field the entry omits is a wildcard.
 */
#include <core.p4>
#include <v1model.p4>

header ethernet_t { bit<48> dstAddr; bit<48> srcAddr; bit<16> etherType; }
header ipv4_t {
    bit<8>  versionIhl; bit<8>  diffserv;  bit<16> totalLen;  bit<16> identification;
    bit<16> flagsFrag;  bit<8>  ttl;       bit<8>  protocol;  bit<16> hdrChecksum;
    bit<32> srcAddr;    bit<32> dstAddr;
}
struct headers { ethernet_t ethernet; ipv4_t ipv4; }
struct metadata { bit<8> mode; bit<16> state; bit<1> marked; }

parser MyParser(packet_in pkt, out headers hdr, inout metadata meta,
                inout standard_metadata_t standard_metadata) {
    state start {
        pkt.extract(hdr.ethernet);
        transition select (hdr.ethernet.etherType) { 0x0800: parse_ipv4; default: accept; }
    }
    state parse_ipv4 { pkt.extract(hdr.ipv4); transition accept; }
}

control MyVerifyChecksum(inout headers hdr, inout metadata meta) { apply {} }

control MyIngress(inout headers hdr, inout metadata meta,
                  inout standard_metadata_t standard_metadata) {
    register<bit<16>>(256) state_reg;

    action set_mode(bit<8> m) { meta.mode = m; }
    table cfg {
        actions = { set_mode; NoAction; }
        default_action = NoAction();
        size = 1;
    }

    action fwd(bit<9> port) { standard_metadata.egress_spec = port; }
    action drop() { mark_to_drop(standard_metadata); }
    table sink_tbl {
        key = { meta.state : exact; }
        actions = { fwd; drop; }
        default_action = drop();
        size = 16;
    }

    action mark() { meta.marked = 1; }
    table aux_tbl {
        key = { hdr.ethernet.etherType : exact; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }

    table opt_tbl {
        key = { hdr.ethernet.srcAddr : optional; hdr.ethernet.dstAddr : optional; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }

    apply {
        opt_tbl.apply();
        cfg.apply();
        if (hdr.ipv4.isValid() && meta.mode == 7) {
            bit<32> idx = (bit<32>)hdr.ipv4.diffserv;
            if (hdr.ipv4.protocol == 17) {
                state_reg.write(idx, hdr.ipv4.identification);
                drop();
            } else {
                state_reg.read(meta.state, idx);
                sink_tbl.apply();
            }
        }
        aux_tbl.apply();
    }
}

control MyEgress(inout headers hdr, inout metadata meta,
                 inout standard_metadata_t standard_metadata) { apply {} }
control MyComputeChecksum(inout headers hdr, inout metadata meta) { apply {} }
control MyDeparser(packet_out pkt, in headers hdr) { apply { pkt.emit(hdr); } }

V1Switch(MyParser(), MyVerifyChecksum(), MyIngress(), MyEgress(), MyComputeChecksum(),
         MyDeparser()) main;
