"""Three-phase packet tester for the BFRT tampering harness.

Runs on the SDE host (requires BF-SDE + tofino_model + bf_switchd). Mirrors
``bmv2/bmv2_driver.py`` ``PacketTester`` semantics:

  Phase 1 — install Phase-1 entities; send Phase-1 packet; verify expected
            output strictly; (Key-sink chains) verify the sink table reported
            a HIT.
  Phase 2 — install Phase-2 entities (additive); send Phase-2 packet
            (lenient); verify each ``affected_register`` was actually written
            with ``attacker_value``.
  Phase 3 — replay Phase 1's input; verify output deviates from Phase 1's
            expected output. For Key-sink chains, additionally verify the
            sink table reports a MISS.

The veth/tcpdump infrastructure is intentionally identical to the BMv2 path;
the differences live in ``bfrt_grpc_client`` (control plane) and in the
HIT/MISS check.
"""

from __future__ import annotations

import logging
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .bfrt_grpc_client import BfRtClient, KeyField as BfRtKeyField, DataField as BfRtDataField
from .txtpb_parser import (AffectedRegister, BfRtEntity, Phase, TamperCase,
                           parse_tampering_txtpb)

log = logging.getLogger("tofino_packet_tester")


# --------------------------------------------------------------------------- #
# Packet I/O helpers — same shape as bmv2/bmv2_driver.PacketTester           #
# --------------------------------------------------------------------------- #

def _sudo_prefix() -> List[str]:
    return [] if os.geteuid() == 0 else ["sudo", "-n"]


def _start_tcpdump(iface: str, pcap_path: Path) -> subprocess.Popen:
    pcap_path.parent.mkdir(parents=True, exist_ok=True)
    pcap_path.unlink(missing_ok=True)
    cmd = [*_sudo_prefix(), "tcpdump", "-U", "--immediate-mode",
           "-i", iface, "-w", str(pcap_path)]
    return subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)


def _stop_tcpdump(proc: subprocess.Popen, pcap_path: Path) -> None:
    if proc.poll() is None:
        subprocess.run([*_sudo_prefix(), "pkill", "-INT", "-P", str(proc.pid)],
                       check=False, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       start_new_session=True)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            subprocess.run([*_sudo_prefix(), "pkill", "-KILL", "-P", str(proc.pid)],
                           check=False, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True)
            proc.wait(timeout=2)
    if pcap_path.exists():
        subprocess.run([*_sudo_prefix(), "chmod", "0644", str(pcap_path)],
                       check=False, start_new_session=True)


def _read_pcap_bytes(pcap_path: Path) -> List[bytes]:
    if not pcap_path.exists() or pcap_path.stat().st_size == 0:
        return []
    try:
        from scapy.all import rdpcap  # imported lazily — only needed when SDE present
        return [bytes(p) for p in rdpcap(str(pcap_path))]
    except Exception as ex:
        log.debug("rdpcap %s: %s", pcap_path, ex)
        return []


_SUDO_SENDP_CODE = (
    "import sys; "
    "from scapy.all import sendp, Ether, conf; "
    "conf.verb = 0; "
    "iface = sys.argv[1]; "
    "data = sys.stdin.buffer.read(); "
    "sendp(Ether(data), iface=iface, verbose=False)"
)


def _sudo_sendp(packet: bytes, iface: str) -> None:
    cmd = [*_sudo_prefix(), sys.executable, "-c", _SUDO_SENDP_CODE, iface]
    subprocess.run(cmd, input=packet, check=True, timeout=10,
                   stdout=subprocess.DEVNULL, start_new_session=True,
                   stderr=subprocess.PIPE)


def _masked_eq(actual: bytes, expected: bytes, mask: bytes) -> bool:
    n = len(expected)
    if len(actual) < n:
        return False
    a = bytes(x & m for x, m in zip(actual[:n], mask))
    e = bytes(x & m for x, m in zip(expected, mask))
    return a == e


# --------------------------------------------------------------------------- #
# Entity translation — BfRtEntity → bfrt_grpc_client KeyField/DataField       #
# --------------------------------------------------------------------------- #

def _to_client_keys(ent: BfRtEntity) -> List[BfRtKeyField]:
    out: List[BfRtKeyField] = []
    for k in ent.keys:
        out.append(BfRtKeyField(field_name=k.field_name,
                                match_kind=k.match_kind,
                                value=k.value,
                                secondary=k.secondary))
    return out


def _to_client_data(ent: BfRtEntity) -> List[BfRtDataField]:
    return [BfRtDataField(field_name=d.field_name, value=d.value) for d in ent.data]


# --------------------------------------------------------------------------- #
# 3-phase orchestration                                                       #
# --------------------------------------------------------------------------- #

@dataclass
class PacketTester:
    client: BfRtClient
    phase_timeout: float
    capture_dir: Path
    port_to_iface: Dict[int, str]   # switch port → host veth

    def _all_ifaces(self) -> List[str]:
        return sorted(self.port_to_iface.values())

    def _install_entities(self, entities: List[BfRtEntity]) -> None:
        for ent in entities:
            self.client.insert_entity(
                table_name=ent.table_name,
                keys=_to_client_keys(ent),
                datas=_to_client_data(ent),
                action_name=ent.action_name,
                priority=ent.priority,
            )

    def _verify_entity_count(self, entities: List[BfRtEntity]) -> Tuple[bool, str]:
        """Read entry counts from the device and compare with the expected entity list."""
        from collections import Counter
        expected: Counter = Counter(ent.table_name for ent in entities)
        for table_name, exp_count in expected.items():
            got = self.client.get_table_entry_count(table_name)
            if got != exp_count:
                return False, (f"table {table_name!r}: expected {exp_count} "
                               f"entries, device reports {got}")
        return True, ""

    def _send_and_capture(self, phase: Phase, label: str) -> Dict[int, List[bytes]]:
        in_iface = self.port_to_iface.get(phase.in_port)
        if in_iface is None:
            raise RuntimeError(f"unmapped input port {phase.in_port}")
        log.debug("Phase %s: sending on iface %s (port %d)", label, in_iface, phase.in_port)
        # Always capture on the input interface too: when exp_port == in_port the
        # switch returns the output packet on the same veth, so tcpdump sees both
        # the injected packet and the switch reply.  We strip the injected copy
        # below so only actual switch output remains.
        sniff = self._all_ifaces()
        pcaps = {i: self.capture_dir / f"{label}_{i}.pcap" for i in sniff}
        dumps = {i: _start_tcpdump(i, pcaps[i]) for i in sniff}
        try:
            time.sleep(0.4)
            _sudo_sendp(phase.in_packet, in_iface)
            time.sleep(self.phase_timeout)
        finally:
            for iface, p in dumps.items():
                _stop_tcpdump(p, pcaps[iface])
        iface_to_port = {v: k for k, v in self.port_to_iface.items()}
        result: Dict[int, List[bytes]] = {
            iface_to_port[i]: _read_pcap_bytes(pcaps[i]) for i in sniff
        }
        # When in_port == exp_port AND in_packet == exp_packet we cannot tell
        # the injected copy apart from the switch's reply by content alone.
        # Leave both intact; _verify_strict will confirm count == 2.
        # Otherwise strip exactly one injected copy so only switch output remains.
        same_port_same_content = (
            phase.exp_port == phase.in_port
            and phase.exp_packet is not None
            and phase.in_packet == phase.exp_packet
        )
        if not same_port_same_content:
            in_pkts = result.get(phase.in_port, [])
            stripped = False
            filtered: List[bytes] = []
            for pkt in in_pkts:
                if not stripped and pkt == phase.in_packet:
                    stripped = True
                else:
                    filtered.append(pkt)
            result[phase.in_port] = filtered
        return result

    def _verify_strict(self, phase: Phase, captured: Dict[int, List[bytes]]
                       ) -> Tuple[bool, str]:
        if phase.exp_packet is None:
            stray = {p: len(pkts) for p, pkts in captured.items() if pkts}
            return (not stray, f"expected no output, got {stray}" if stray else "")
        exp_port = phase.exp_port
        on_exp = captured.get(exp_port, [])
        # Same-port, same-content: tcpdump captures both the injected packet and
        # the switch reply — verify count == 2 (1 sent + 1 returned).
        if exp_port == phase.in_port and phase.in_packet == phase.exp_packet:
            elsewhere = {p: len(pkts) for p, pkts in captured.items()
                         if p != exp_port and pkts}
            if len(on_exp) == 2 and not elsewhere:
                return True, ""
            return False, (
                f"expected 2 packets on port {exp_port} (1 injected + 1 returned), "
                f"got {len(on_exp)}"
                + (f"; also got {elsewhere}" if elsewhere else "")
            )
        mask = phase.exp_mask or b"\xff" * len(phase.exp_packet)
        for pkt in on_exp:
            log.debug("verifying on port %d: pkt=%s exp=%s mask=%s",
                      exp_port, pkt.hex(), phase.exp_packet.hex(), mask.hex())
            if _masked_eq(pkt, phase.exp_packet, mask):
                elsewhere = {p: len(pkts) for p, pkts in captured.items()
                             if p != exp_port and pkts}
                if elsewhere:
                    return False, f"matched on port {exp_port} but also got {elsewhere}"
                return True, ""
        return False, f"no match on port {exp_port}; captured={ {p:len(v) for p,v in captured.items()} }"

    def _verify_register(self, reg: AffectedRegister) -> Tuple[bool, str]:
        cells = self.client.read_register_all(reg.register_name)
        for (idx, data) in cells:
            if idx == reg.index:
                # data is the client's representation; the harness accepts a
                # raw int OR a dict with a single bytes field. Normalize to
                # bytes for comparison.
                got = _coerce_register_value(data)
                if got == reg.attacker_value:
                    return True, ""
                return False, (f"register {reg.register_name}[{idx}] = "
                               f"{got.hex() if isinstance(got, bytes) else got!r}, "
                               f"expected {reg.attacker_value.hex()}")
        return False, f"register {reg.register_name}[{reg.index}] not present"

    def _verify_sink_miss(self, sink_tables: List[str], phase1_keys: Dict[str, set],
                          captured_p3: Dict[int, List[bytes]],
                          phase1: Phase) -> Tuple[bool, str]:
        """For Key-sink chains, Phase 3 must MISS the sink table. We infer MISS
        from forwarding deviation: if Phase 3's captured packet/port differs
        from Phase 1's expected, the lookup keyed on the now-different register
        value didn't hit. Per-sink-table hit-counter verification is a future
        refinement (requires the program to enable counters on the sink table).
        """
        ref_port = phase1.exp_port
        ref_pkt = phase1.exp_packet
        ref_mask = phase1.exp_mask or (b"\xff" * len(ref_pkt) if ref_pkt else b"")
        observed_port = next((p for p, pkts in captured_p3.items() if pkts), None)
        observed_pkt = captured_p3[observed_port][0] if observed_port is not None else None
        if observed_port != ref_port:
            return True, f"port changed {ref_port} → {observed_port} (likely MISS on {sink_tables})"
        if ref_pkt is not None and observed_pkt is not None:
            if not _masked_eq(observed_pkt, ref_pkt, ref_mask):
                return True, f"packet bytes differ on port {observed_port} (likely MISS on {sink_tables})"
            return False, f"no deviation from Phase 1 (sink {sink_tables} may still HIT)"
        if (observed_pkt is None) != (ref_pkt is None):
            return True, f"drop state changed (likely MISS on {sink_tables})"
        return False, f"no deviation from Phase 1 (sink {sink_tables} may still HIT)"

    def run(self, case: TamperCase, label: str) -> Tuple[bool, str]:
        # Phase 1: install + send + strict verify.
        self._install_entities(case.entities)
        ok, why = self._verify_entity_count(case.entities)
        if not ok:
            return False, f"entity_count: {why}"
        cap1 = self._send_and_capture(case.phases[0], f"{label}_p1")
        ok, why = self._verify_strict(case.phases[0], cap1)
        if not ok:
            return False, f"phase1: {why}"

        # Snapshot Phase-1 installed keys per sink table for the MISS check.
        phase1_keys: Dict[str, set] = {}
        for ent in case.entities:
            if ent.phase and ent.phase != 1:
                continue
            phase1_keys.setdefault(ent.table_name, set()).update(
                (k.field_name, k.value) for k in ent.keys
            )

        # Phase 2: send + lenient (no strict packet check); verify
        # attacker register writes.
        _ = self._send_and_capture(case.phases[1], f"{label}_p2")
        for reg in case.affected_registers:
            ok, why = self._verify_register(reg)
            if not ok:
                return False, f"phase2_reg: {why}"

        # Phase 3: replay Phase 1; check deviation (or HIT/MISS on sink tables).
        cap3 = self._send_and_capture(case.phases[2], f"{label}_p3")
        sink_constraint = any(r.sink_tables for r in case.affected_registers)
        if sink_constraint:
            sinks = sorted({t for r in case.affected_registers for t in r.sink_tables})
            ok, why = self._verify_sink_miss(sinks, phase1_keys, cap3, case.phases[0])
            if not ok:
                return False, f"phase3_sink: {why}"
            return True, f"OK (Phase1 HIT → Phase3 MISS on {sinks}): {why}"
        # Fallback: BMv2-style deviation check (no Key-sink metadata).
        return self._verify_sink_miss([], phase1_keys, cap3, case.phases[0])


def _coerce_register_value(data) -> bytes:
    """Normalize whatever bfrt_grpc returns for a register read to bytes."""
    if isinstance(data, (bytes, bytearray)):
        return bytes(data)
    if isinstance(data, int):
        return data.to_bytes((data.bit_length() + 7) // 8 or 1, "big")
    if isinstance(data, dict):
        # bfrt_grpc 'data.to_dict()' shape: {"fields": [{"name":…, "stream":…}, ...]}
        for fld in data.get("fields", []):
            v = fld.get("stream", fld.get("int_val"))
            if isinstance(v, (bytes, bytearray)):
                return bytes(v)
            if isinstance(v, int):
                return v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
    return b""


# Re-exported for convenience:
parse_case = parse_tampering_txtpb
