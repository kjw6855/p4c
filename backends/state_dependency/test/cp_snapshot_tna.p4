/*
 * --cp-snapshot fixture (TNA). The control plane decides everything that makes the H2S2K/OutK
 * chain state_reg -> sink_tbl reachable:
 *   - cfg is keyless; only its RUNTIME default set_mode(7) opens the stateful path,
 *   - sink_tbl forwards only the key the controller installed (0x0800 -> port 2), else drops,
 *   - state_reg[5] holds 0x0800 because the controller wrote it, so a victim reading cell 5 HITs.
 * The attacker's write is gated on ttl == gate_reg[0], a cell only the controller sets (0x40):
 * the case must be tagged "Register knowledge: required" naming that cell.
 * pri_tbl, lpm_tbl and rng_tbl exercise the keyset forms and the evaluation order (ternary
 * priority with a $valid key, LPM, range); the test reads the materialized order from the log.
 * aux_tbl is never mentioned by the snapshot: closed world leaves it empty (default only).
 * An IPv4 packet with protocol 17 writes the cell (the attacker); any other reads it (the victim).
 * Expected with cp_snapshot_tna.txtpb: HIT->MISS cases with the attacker on port 3 and the victim on
 * a host port (1 or 2). Without the register cell (cp_snapshot_tna_noreg.txtpb) the victim reads 0
 * and MISSes, so no HIT->MISS case exists.
 */
#include <core.p4>
#include <tna.p4>

header ethernet_h { bit<48> dst_addr; bit<48> src_addr; bit<16> ether_type; }
header ipv4_h {
    bit<8>  version_ihl; bit<8>  diffserv;  bit<16> total_len;  bit<16> identification;
    bit<16> flags_frag;  bit<8>  ttl;       bit<8>  protocol;   bit<16> hdr_checksum;
    bit<32> src_addr;    bit<32> dst_addr;
}

struct headers_t { ethernet_h ethernet; ipv4_h ipv4; }
struct ig_metadata_t { bit<8> mode; bit<16> state; bit<1> marked; }
struct eg_metadata_t {}

parser IngressParser(packet_in pkt, out headers_t hdr, out ig_metadata_t ig_md,
                     out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        pkt.advance(PORT_METADATA_SIZE);
        ig_md.mode = 0;
        ig_md.state = 0;
        ig_md.marked = 0;
        transition parse_ethernet;
    }
    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select (hdr.ethernet.ether_type) { 0x0800: parse_ipv4; default: accept; }
    }
    state parse_ipv4 { pkt.extract(hdr.ipv4); transition accept; }
}

control Ingress(inout headers_t hdr, inout ig_metadata_t ig_md,
                in    ingress_intrinsic_metadata_t              ig_intr_md,
                in    ingress_intrinsic_metadata_from_parser_t  ig_prsr_md,
                inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
                inout ingress_intrinsic_metadata_for_tm_t       ig_tm_md) {

    Register<bit<16>, bit<8>>(256, 0) state_reg;
    RegisterAction<bit<16>, bit<8>, bit<16>>(state_reg) read_state = {
        void apply(inout bit<16> v, out bit<16> rv) { rv = v; }
    };
    RegisterAction<bit<16>, bit<8>, bit<16>>(state_reg) write_state = {
        void apply(inout bit<16> v) { v = hdr.ipv4.identification; }
    };
    Register<bit<8>, bit<1>>(1, 0) gate_reg;
    RegisterAction<bit<8>, bit<1>, bit<8>>(gate_reg) read_gate = {
        void apply(inout bit<8> v, out bit<8> rv) { rv = v; }
    };

    action set_mode(bit<8> m) { ig_md.mode = m; }
    table cfg {
        actions = { set_mode; NoAction; }
        default_action = NoAction();
        size = 1;
    }

    action fwd(PortId_t port) { ig_tm_md.ucast_egress_port = port; }
    action drop() { ig_dprsr_md.drop_ctl = 0x1; }
    table sink_tbl {
        key = { ig_md.state : exact; }
        actions = { fwd; drop; }
        default_action = drop();
        size = 16;
    }

    action mark() { ig_md.marked = 1; }
    table aux_tbl {
        key = { hdr.ethernet.ether_type : exact; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }

    table pri_tbl {
        key = { hdr.ipv4.ttl : ternary; hdr.ipv4.isValid() : exact; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }
    table lpm_tbl {
        key = { hdr.ipv4.dst_addr : lpm; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }
    table rng_tbl {
        key = { hdr.ipv4.total_len : range; }
        actions = { mark; NoAction; }
        default_action = NoAction();
        size = 16;
    }

    apply {
        pri_tbl.apply();
        lpm_tbl.apply();
        rng_tbl.apply();
        cfg.apply();
        if (hdr.ipv4.isValid() && ig_md.mode == 7) {
            bit<8> idx = hdr.ipv4.diffserv;
            if (hdr.ipv4.protocol == 17) {
                bit<8> gate = read_gate.execute(0);
                if (hdr.ipv4.ttl == gate) {
                    write_state.execute(idx);
                }
                drop();
            } else {
                ig_md.state = read_state.execute(idx);
                sink_tbl.apply();
            }
        }
        aux_tbl.apply();
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
