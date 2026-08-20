/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

#include <core.p4>
// Tofino1 only: the TNA spelling of predicate() takes (cmplo, cmphi), whereas T2NA takes
// cmp0..cmp3. Keeping this out of p4-programs/ stops the T2NA suites from compiling it.
#include <tna.p4>

#include "../common/headers.p4"
#include "../common/util.p4"

struct metadata_t {}

Register<bit<32>, bit<11>>(2048) ping_reg;


// ---------------------------------------------------------------------------
// Ingress parser
// ---------------------------------------------------------------------------
parser SwitchIngressParser(
        packet_in pkt,
        out header_t hdr,
        out metadata_t ig_md,
        out ingress_intrinsic_metadata_t ig_intr_md) {
    TofinoIngressParser() tofino_parser;

    state start {
        tofino_parser.apply(pkt, ig_intr_md);
        transition parse_ethernet;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition parse_ipv4;
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition accept;
    }
}

// ---------------------------------------------------------------------------
// Ingress Deparser
// ---------------------------------------------------------------------------
control SwitchIngressDeparser(
        packet_out pkt,
        inout header_t hdr,
        in metadata_t ig_md,
        in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {
    apply {
        pkt.emit(hdr);
    }
}

control SwitchIngress(
        inout header_t h,
        inout metadata_t ig_md,
        in ingress_intrinsic_metadata_t ig_intr_md,
        in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
        inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
        inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {

    RegisterAction<bit<32>, bit<11>, bit<32>>(ping_reg) ping_update = {
        void apply(inout bit<32> value) { value = 0xFFFFFFFF; } };

    RegisterAction<bit<32>, bit<11>, bit<32>>(ping_reg) ping_get = {
        void apply(inout bit<32> value, out bit<32> rv) { rv = value; }
    };

    // The stateful ALU's one-hot predicate output, indexed by cmplo + 2 * cmphi:
    //   cmplo=0 cmphi=0 -> 1,  cmplo=1 cmphi=0 -> 2,
    //   cmplo=0 cmphi=1 -> 4,  cmplo=1 cmphi=1 -> 8.
    // The comparisons are driven by the packet rather than by the register: a register read is
    // modelled as taint outside the state-dependency flow (the pre-existing
    // tna_register_action_1.p4 shows the same), which would make every outcome tainted and test
    // nothing. Driving them from header fields keeps all four combinations reachable and exact.
    RegisterAction<bit<32>, bit<11>, bit<8>>(ping_reg) ping_predicate = {
        void apply(inout bit<32> value, out bit<8> rv) {
            rv = this.predicate(h.ipv4.dst_addr != 0, h.ipv4.dst_addr > 0x7FFFFFFF);
        }
    };

    apply {
        ping_update.execute(1);

        h.ipv4.src_addr = ping_get.execute(1);
        if (h.ipv4.src_addr == 0) {
            h.ipv4.src_addr = 1;
        }
        // Make the predicate output observable in the emitted packet.
        h.ipv4.ttl = ping_predicate.execute(7);
        if (h.ipv4.ttl >= 4) {
            h.ipv4.diffserv = 0xF0;
        } else if (h.ipv4.ttl == 2) {
            h.ipv4.diffserv = 0x0F;
        }
        h.ethernet.dst_addr = 0xAAAAAAAAAAAA;
        h.ethernet.src_addr =  0xBBBBBBBBBBBB;
        h.ethernet.ether_type =  0xCCCC;
        // We also need to explicitly set the output port.
        ig_tm_md.ucast_egress_port = 8;
    }
}

parser SwitchEgressParser(
        packet_in pkt,
        out header_t hdr,
        out metadata_t eg_md,
        out egress_intrinsic_metadata_t eg_intr_md) {

    state start {
        pkt.extract(eg_intr_md);
        pkt.extract(hdr.ethernet);
        pkt.extract(hdr.ipv4);
        transition accept;
    }
}

control SwitchEgress(
        inout header_t hdr,
        inout metadata_t eg_md,
        in egress_intrinsic_metadata_t eg_intr_md,
        in egress_intrinsic_metadata_from_parser_t eg_intr_md_from_prsr,
        inout egress_intrinsic_metadata_for_deparser_t ig_intr_dprs_md,
        inout egress_intrinsic_metadata_for_output_port_t eg_intr_oport_md) {
    apply {
    }
}

control SwitchEgressDeparser(
        packet_out pkt,
        inout header_t hdr,
        in metadata_t eg_md,
        in egress_intrinsic_metadata_for_deparser_t ig_intr_dprs_md) {
    apply {
        pkt.emit(hdr.ethernet);
        pkt.emit(hdr.ipv4);
    }
}

Pipeline(SwitchIngressParser(),
         SwitchIngress(),
         SwitchIngressDeparser(),
         SwitchEgressParser(),
         SwitchEgress(),
         SwitchEgressDeparser()) pipe;

Switch(pipe) main;
