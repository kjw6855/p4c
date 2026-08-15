/*
 * Const-entry ACTION-DIVERGENCE fixture (fisslock lock_operation shape), v1model / BMv2.
 *
 * Story (H2S2K, split write/read across two packets, exactly fisslock's structure):
 *   attacker packet (protocol == 6)  -> mode_reg.write(idx, chosen bit)
 *   victim   packet (protocol != 6)  -> mode_reg.read(meta.mode, idx); sink_tbl.apply()
 * The victim purely READS; it never writes the cell it reads. That split is what makes the tamper
 * attributable to the attacker rather than to the victim's own packet, and it is the property the
 * self-set-register false positives (Mew, linkguardian h2m) lack.
 *
 * Why this shape and not a HIT/MISS fixture: `sink_tbl`'s two const entries cover its ENTIRE 1-bit
 * key space, so the table can NEVER MISS. HIT->MISS and MISS->HIT are structurally inapplicable --
 * constEntriesCoverKeySpace suppresses both, and this program emits ZERO without
 * --const-entry-action-divergence. Moving the key from one const entry to the other is the only
 * tamper that exists here, which makes the A/B unambiguous: 0 without the flag, adiv-only with it.
 *
 * Why BOTH entries name the SAME action: `set_out` differing only in its ARGUMENTS is the case the
 * action stamp alone cannot resolve -- both entries stamp `set_out` -- so this is what exercises the
 * reader's concrete-key tier (readConstEntrySelection tier 2 in state_dependency_track.cpp). It is
 * also the case that only holds water for const entries: `set_out(port=1)` vs `set_out(port=2)` is a
 * genuine, program-fixed traffic redirection here, whereas on a control-plane table both argument
 * values would have been invented by p4symbex itself.
 *
 * The two entries differ in BOTH the egress port AND an emitted header byte (ttl), deliberately:
 * the replay oracle reduces a run to (port, first_packet_bytes), so belt-and-braces against a
 * "diverges in the model, identical on the wire" outcome.
 *
 * v1model register .read()/.write() is used rather than an extern object: the analyzer models those
 * two methods directly (backends/state_dependency/controls.cpp), whereas a Tofino RegisterAction
 * goes through a documented interprocedural gap.
 *
 * STATUS (2026-08-15): this fixture currently emits 0 WITH the flag, and it is kept as the
 * reproducer for exactly why. The pass runs correctly here -- both HIT/MISS directions suppressed,
 * Phase-1 terminals matched a const entry, the legit Phase-3 outcome reads as
 * `set_out(port=1,tag=170)` -- but the attack Phase-3 outcome comes back IDENTICAL, because the
 * search reaches it through the accumulation driver, which replays the same Phase-2 packet. Against
 * a plain `write()` of an attacker-chosen value that replay is idempotent: the goal only ever sees
 * the value the Phase-2 solver picked, never the --state-tamper-value the emission path would have
 * forced into the packet. fisslock emits fine because its RegisterAction is read-modify-write, so
 * replaying does move the value. Unblocking this needs the written VALUE steered toward one
 * selecting a different const entry; see SymbexOptions::constEntryActionDivergence.
 *
 * Expected once that lands:
 *   analysis   `p4c_state_dependency --arch v1model --supergraph FULL`: DATA writes to key >= 1
 *              (this already holds today)
 *   generation without the flag: 0 txtpb (suppressed, "can never MISS") -- already holds
 *   generation with    the flag: >= 1 txtpb named *_adiv.txtpb, carrying two DIFFERENT non-empty
 *                                sink_outcome_legit / sink_outcome_attack strings and NO
 *                                hit_phase / miss_phase; then replayable end-to-end on BMv2, which
 *                                is the only target that can replay on this machine.
 */
#include <core.p4>
#include <v1model.p4>

const bit<16> TYPE_IPV4 = 0x0800;
const bit<8>  PROTO_ATTACKER = 6;

header ethernet_t {
    bit<48> dstAddr;
    bit<48> srcAddr;
    bit<16> etherType;
}

header ipv4_t {
    bit<4>  version;
    bit<4>  ihl;
    bit<8>  diffserv;        // register index: the attacker picks the victim's cell with this
    bit<16> totalLen;
    bit<16> identification;
    bit<3>  flags;
    bit<13> fragOffset;
    bit<8>  ttl;             // written by the sink action -> the divergence is visible on the wire
    bit<8>  protocol;        // 6 = attacker (writes), anything else = victim (reads)
    bit<16> hdrChecksum;
    bit<32> srcAddr;
    bit<32> dstAddr;
}

/* Attacker-only trailer. `mode_val` is a WHOLE header field, written into the register with no
 * masking or casting, so the value is directly packet-injectable: p4symbex can solve for the byte
 * that makes the victim select the other const entry. An arithmetic form such as
 * `(bit<1>)(hdr.ipv4.identification & 1)` is NOT injectable -- generation then reports
 * "program-fixed write value (not packet-injectable)", keeps whatever value the path happened to
 * produce, and the divergence goal can never be satisfied. */
header ctrl_t {
    bit<1> mode_val;
    bit<7> pad;
}

struct metadata {
    bit<1> mode;             // read out of mode_reg; IS the sink key
}

struct headers {
    ethernet_t ethernet;
    ipv4_t     ipv4;
    ctrl_t     ctrl;
}

parser MyParser(packet_in packet, out headers hdr, inout metadata meta,
                inout standard_metadata_t standard_metadata) {
    state start {
        packet.extract(hdr.ethernet);
        transition select(hdr.ethernet.etherType) {
            TYPE_IPV4: parse_ipv4;
            default:   accept;
        }
    }
    state parse_ipv4 {
        packet.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol) {
            PROTO_ATTACKER: parse_ctrl;
            default:        accept;
        }
    }
    state parse_ctrl {
        packet.extract(hdr.ctrl);
        transition accept;
    }
}

control MyVerifyChecksum(inout headers hdr, inout metadata meta) {
    apply {}
}

control MyIngress(inout headers hdr, inout metadata meta,
                  inout standard_metadata_t standard_metadata) {

    register<bit<1>>(256) mode_reg;

    // ONE action, two const entries, different arguments. Both the port and the emitted ttl differ.
    action set_out(bit<9> port, bit<8> tag) {
        standard_metadata.egress_spec = port;
        hdr.ipv4.ttl = tag;
    }

    action drop_pkt() {
        mark_to_drop(standard_metadata);
    }

    table sink_tbl {
        key = {
            meta.mode : exact;
        }
        actions = { set_out; drop_pkt; }
        // Two entries over a 1-bit exact key: the whole key space is named, so this table can never
        // MISS. Keep it that way -- a partial map would let the HIT/MISS passes back in and make the
        // A/B ambiguous.
        const entries = {
            1w0 : set_out(1, 0xAA);
            1w1 : set_out(2, 0xBB);
        }
        const default_action = drop_pkt();
        size = 2;
    }

    apply {
        // `diffserv == 0` gates the whole pipeline, so both packets provably address cell 0.
        //
        // The index MUST stay a header field: with a literal constant the state-dependency analysis
        // forms no chain at all ("no chains for the selected policy"), because a register chain
        // anchors on a header-sourced index. But leaving it free would put the attacker on whatever
        // cell the solver liked, and the tamper would then write a cell the victim never reads --
        // which is a question about index steering, not about the const-entry outcome under test.
        // Constraining the field is how this fixture keeps the anchor without the ambiguity.
        if (hdr.ipv4.isValid() && hdr.ipv4.diffserv == 8w0) {
            bit<32> idx = (bit<32>)hdr.ipv4.diffserv;
            if (hdr.ipv4.protocol == PROTO_ATTACKER && hdr.ctrl.isValid()) {
                // Attacker: writes the cell the victim will read. Nothing else.
                mode_reg.write(idx, hdr.ctrl.mode_val);
                mark_to_drop(standard_metadata);
            } else {
                // Victim: pure read, straight into the sink key.
                mode_reg.read(meta.mode, idx);
                sink_tbl.apply();
            }
        }
    }
}

control MyEgress(inout headers hdr, inout metadata meta,
                 inout standard_metadata_t standard_metadata) {
    apply {}
}

control MyComputeChecksum(inout headers hdr, inout metadata meta) {
    apply {}
}

control MyDeparser(packet_out packet, in headers hdr) {
    apply {
        packet.emit(hdr.ethernet);
        packet.emit(hdr.ipv4);
        packet.emit(hdr.ctrl);
    }
}

V1Switch(MyParser(), MyVerifyChecksum(), MyIngress(), MyEgress(), MyComputeChecksum(),
         MyDeparser()) main;
