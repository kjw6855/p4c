// Fixture for guard_overwrite_test.sh: what happens to a condition chain when the write it guards
// is overwritten later. Each register's read feeds exactly one condition, so the register name
// identifies the case in the chain cache.
//
// The P4 frontend (SimplifyDefUse, then SimplifyControlFlow) deletes a guarded write that every
// path overwrites before any read. The `if` becomes empty and is removed, and so is the now-unused
// register read. So r_dead and r_out have no condition left to chain to (the same holds for the
// compiled program); every other case keeps its condition.
#include <core.p4>
#include <v1model.p4>

header h_t { bit<32> a; bit<32> b; }
struct headers_t { h_t h; }
struct meta_t {
    bit<8> a; bit<8> b; bit<8> c; bit<8> d; bit<8> e; bit<8> f; bit<8> g; bit<8> h; bit<8> i;
}

parser P(packet_in pkt, out headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    state start { pkt.extract(hdr.h); transition accept; }
}
control V(inout headers_t hdr, inout meta_t meta) { apply { } }

control I(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) {
    register<bit<32>>(16) r_ctrl;     // guarded write, never overwritten
    register<bit<32>>(16) r_dead;     // guarded write, unconditionally overwritten later
    register<bit<32>>(16) r_read;     // guarded write, read, then overwritten
    register<bit<32>>(16) r_condow;   // guarded write, overwritten only under another condition
    register<bit<32>>(16) r_else;     // both branches write the same variable
    register<bit<32>>(16) r_local;    // local variable initialised inside the if block
    register<bit<32>>(16) r_actow;    // guarded write, overwritten by an action of a later table
    register<bit<32>>(16) r_out;      // guarded output write, unconditionally overwritten later
    register<bit<32>>(16) r_outread;  // guarded output write, read by an output condition, then overwritten

    action ow_h() { meta.h = 7; }
    table t_ow { key = { hdr.h.b: exact; } actions = { ow_h; NoAction; } default_action = NoAction(); }

    apply {
        bit<32> v;
        r_ctrl.write(hdr.h.a, hdr.h.a);   r_ctrl.read(v, hdr.h.a);
        if (v == 1) { meta.a = 1; }

        r_dead.write(hdr.h.a, hdr.h.a);   r_dead.read(v, hdr.h.a);
        if (v == 2) { meta.b = 1; }
        meta.b = 7;

        r_read.write(hdr.h.a, hdr.h.a);   r_read.read(v, hdr.h.a);
        if (v == 3) { meta.c = 1; }
        meta.d = meta.c;
        meta.c = 7;

        r_condow.write(hdr.h.a, hdr.h.a); r_condow.read(v, hdr.h.a);
        if (v == 4) { meta.e = 1; }
        if (hdr.h.b == 9) { meta.e = 7; }

        r_else.write(hdr.h.a, hdr.h.a);   r_else.read(v, hdr.h.a);
        if (v == 5) { meta.g = 1; } else { meta.g = 2; }

        r_local.write(hdr.h.a, hdr.h.a);  r_local.read(v, hdr.h.a);
        if (v == 6) { bit<8> t = hdr.h.b[7:0]; meta.f = t; }

        r_actow.write(hdr.h.a, hdr.h.a);  r_actow.read(v, hdr.h.a);
        if (v == 7) { meta.h = 1; }
        t_ow.apply();

        r_out.write(hdr.h.a, hdr.h.a);    r_out.read(v, hdr.h.a);
        if (v == 8) { sm.egress_spec = 2; }
        sm.egress_spec = 5;

        r_outread.write(hdr.h.a, hdr.h.a); r_outread.read(v, hdr.h.a);
        if (v == 9) { sm.egress_spec = 3; }
        if (sm.egress_spec == 3) { sm.mcast_grp = 1; }
        sm.egress_spec = 6;
    }
}
control E(inout headers_t hdr, inout meta_t meta, inout standard_metadata_t sm) { apply { } }
control C(inout headers_t hdr, inout meta_t meta) { apply { } }
control D(packet_out pkt, in headers_t hdr) { apply { pkt.emit(hdr.h); } }
V1Switch(P(), V(), I(), E(), C(), D()) main;
