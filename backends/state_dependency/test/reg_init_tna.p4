/*
 * Declared-initial-value fixture (R3).
 *
 * Identical in shape to lock_flip_tna.p4 except the register declares a NON-ZERO initial value:
 *     Register<bit<8>, bit<8>>(256, 7) lock_reg;
 *
 * A cell no packet has written must therefore read as 7, so Phase-1's check HITs lock_tbl on
 * key=7 and {7} -- not {0} -- is the forbidden set. Seeding 0 (the old "hardware default"
 * assumption) would put the Phase-1 HIT on key 0 instead, which is exactly the SwitchV2P
 * cache-hit-on-an-empty-slot false positive in miniature.
 *
 * A zero-init fixture cannot detect this: 0 is what both the old and new code produce.
 */
#include <core.p4>
#include <tna.p4>

header ethernet_h { bit<48> dst_addr; bit<48> src_addr; bit<16> ether_type; }
header ipv4_h {
    bit<8>  version_ihl; bit<8>  diffserv;  bit<16> total_len;  bit<16> identification;
    bit<16> flags_frag;  bit<8>  ttl;       bit<8>  protocol;   bit<16> hdr_checksum;
    bit<32> src_addr;    bit<32> dst_addr;
}

// A bit<1> control field the attacker sets directly, so the register write value is a bare
// symbolic variable (packet-injectable) rather than a slice/expression.
header ctrl_h { bit<8> lock_val8; bit<7> pad; }

struct headers_t { ethernet_h ethernet; ipv4_h ipv4; ctrl_h ctrl; }
struct ig_metadata_t { bit<8> lock_val; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        transition parse_ethernet;
    }
    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select (hdr.ethernet.ether_type) {
            0x0800:  parse_ipv4;
            default: accept;
        }
    }
    state parse_ipv4 { pkt.extract(hdr.ipv4); transition parse_ctrl; }
    state parse_ctrl { pkt.extract(hdr.ctrl); transition accept; }
}

control Ingress(inout headers_t hdr, inout ig_metadata_t ig_md,
                in    ingress_intrinsic_metadata_t              ig_intr_md,
                in    ingress_intrinsic_metadata_from_parser_t  ig_prsr_md,
                inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                inout ingress_intrinsic_metadata_for_tm_t       ig_tm_md) {

    // bit<1> locks indexed by a HEADER-SOURCED index (per-flow lock). The chain anchors on the
    // header-sourced index (a constant index does not anchor — see reference RegisterAction data
    // path gap). Cells init to 0; Phase-1's check reads 0 and HITs lock_tbl on key=0, so the
    // Phase-1 HIT key {0} is the forbidden set.
    Register<bit<8>, bit<8>>(256, 7) lock_reg;   // NON-ZERO declared initial value
    // "set" packet writes the lock from a bit<1> header field DIRECTLY, so the recorded write value
    // is a bare symbolic variable (packet-injectable) and withAttackerValues selects the tamper
    // value / applies the forbidden-value override.
    RegisterAction<bit<8>, bit<8>, bit<8>>(lock_reg) set_lock = {
        void apply(inout bit<8> v) { v = hdr.ctrl.lock_val8; }
    };
    // "check" packet reads the lock back into the table match key.
    RegisterAction<bit<8>, bit<8>, bit<8>>(lock_reg) get_lock = {
        void apply(inout bit<8> v, out bit<8> r) { r = v; }
    };

    action allow(PortId_t port) { ig_tm_md.ucast_egress_port = port; }
    table lock_tbl {
        key     = { ig_md.lock_val : exact; }   // H2S2K sink: register read result -> match key
        actions = { allow; NoAction; }
        default_action = NoAction;
        size = 16;
    }

    apply {
        if (hdr.ipv4.isValid()) {
            bit<8> idx = hdr.ipv4.diffserv;   // header-sourced index (anchors the chain)
            if (hdr.ipv4.ttl == 1) {          // "set" packet: acquire the lock
                set_lock.execute(idx);
            } else {                          // "check" packet (different packet): read -> key
                ig_md.lock_val = get_lock.execute(idx);
                lock_tbl.apply();
            }
        }
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
