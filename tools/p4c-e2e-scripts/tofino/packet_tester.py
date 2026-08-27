"""Three-phase packet tester for the BFRT tampering harness.

Runs on the SDE host (requires BF-SDE + tofino_model + bf_switchd). Mirrors
``bmv2/bmv2_driver.py`` ``PacketTester`` semantics.

The verdict comes from a DIFFERENTIAL, not from per-phase assertions. The
driver resets the device and calls :meth:`PacketTester.replay` twice on the
same case:

  legit run   Phase 1 -> Phase 3          (the victim alone)
  attack run  Phase 1 -> Phase 2 xk -> Phase 3   (attacker packets interposed)

Both runs replay the same Phase-1 and Phase-3 packets, so the only difference
between them is the attacker's Phase-2 write; :meth:`compare_runs` reduces each
run's Phase-3 capture to ``(port, first_packet_bytes)`` (or ``None`` for a drop)
and calls the case VULNERABLE iff the two differ.

It therefore asks nothing about hit/miss. The sink may equally diverge by
running a DIFFERENT action in both runs -- a const-entries table total over its
key bits can never MISS at all -- so ``hit_phase`` / ``miss_phase``, where the
generator emits them, are triage metadata and not a criterion here.
``affected_register`` is likewise read back only as a diagnostic string
(:meth:`_diag_registers`), never as a pass/fail condition.

The veth/tcpdump infrastructure is intentionally identical to the BMv2 path;
the differences live in ``bfrt_grpc_client`` (control plane).
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


# Multi-packet (accumulation) flood tuning. tofino_model is a slow software switch that drops most of
# a back-to-back burst, so we (a) pace the send with a small inter-packet gap to cut the loss and
# (b) read the register and resend the deficit until it reaches the target. Override via env.
FLOOD_INTER = float(os.environ.get("P4CSD_FLOOD_INTER", "0.001"))   # seconds between packets
# Adaptive stop for the resend loop: rather than a fixed round count (which cuts off a slow-but-
# progressing flood before it reaches the target), keep going while the register is advancing toward
# the target and stop only when it PLATEAUS (no net gain for this many consecutive rounds ⇒ packet
# loss is no longer recovering) or REVERSES (drops below the best seen ⇒ cell reset / competing
# writer). FLOOD_MAX_ROUNDS is now only a runaway backstop, not the primary criterion.
FLOOD_STALL_ROUNDS = int(os.environ.get("P4CSD_FLOOD_STALL_ROUNDS", "3"))
FLOOD_MAX_ROUNDS = int(os.environ.get("P4CSD_FLOOD_MAX_ROUNDS", "1000"))

_SUDO_SENDP_CODE = (
    "import sys; "
    "from scapy.all import sendp, Ether, conf; "
    "conf.verb = 0; "
    "iface = sys.argv[1]; "
    "count = int(sys.argv[2]) if len(sys.argv) > 2 else 1; "
    "inter = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0; "
    "data = sys.stdin.buffer.read(); "
    "sendp(Ether(data), iface=iface, count=count, inter=inter, verbose=False)"
)


def _sudo_sendp(packet: bytes, iface: str, count: int = 1, inter: float = 0.0) -> None:
    # `count` copies are sent in a single scapy call (one subprocess), optionally paced by `inter`
    # seconds. Scale the timeout with the count and the pacing.
    cmd = [*_sudo_prefix(), sys.executable, "-c", _SUDO_SENDP_CODE, iface, str(count), str(inter)]
    timeout = max(10, count // 100 + 30) + int(count * inter) + 30
    subprocess.run(cmd, input=packet, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, start_new_session=True,
                   stderr=subprocess.PIPE)


def _masked_eq(actual: bytes, expected: bytes, mask: bytes) -> bool:
    n = len(expected)
    if len(actual) < n:
        return False
    a = bytes(x & m for x, m in zip(actual[:n], mask))
    e = bytes(x & m for x, m in zip(expected, mask))
    return a == e


def _pkt_diff(a: bytes, b: bytes, max_show: int = 12) -> str:
    """Human summary of where two equal-length output packets differ: byte offsets + values
    (legit -> attack). Falls back to a length note when the lengths differ."""
    if len(a) != len(b):
        return f"length {len(a)}B vs {len(b)}B"
    diffs = [(i, a[i], b[i]) for i in range(len(a)) if a[i] != b[i]]
    if not diffs:
        return "identical"
    head = ", ".join(f"@{i}:{x:02x}->{y:02x}" for i, x, y in diffs[:max_show])
    more = f" (+{len(diffs) - max_show} more)" if len(diffs) > max_show else ""
    return f"{len(diffs)} byte(s) [{head}{more}]"


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
    strong_verify: bool = False     # require the attacker value at the exact register index

    def _all_ifaces(self) -> List[str]:
        return sorted(self.port_to_iface.values())

    @staticmethod
    def _entry_match_sig(ent: BfRtEntity):
        """Hashable signature of an entry's MATCH KEY (table + priority + keys). The device holds a
        single table state for all phases, so two entries with the same match key are the same
        physical entry — installing the second would fail with ALREADY_EXISTS."""
        def _hashable(v):
            return bytes(v) if isinstance(v, (bytes, bytearray)) else v
        keys = tuple((k.field_name, k.match_kind, _hashable(k.value), _hashable(k.secondary))
                     for k in ent.keys)
        # is_default_entry is part of the signature because a default entry carries NO keys, so
        # without it a table's default entry and a genuinely keyless match entry on that same table
        # would share a signature and one would be silently dropped.
        return (ent.table_name, ent.priority, keys, ent.is_default_entry)

    def _install_entities(self, entities: List[BfRtEntity]) -> None:
        # Tampering test cases carry per-phase entity blocks (phase 1 and phase 2). Phases share the
        # single installed table state, so entries that are identical across phases (e.g. a sink-table
        # entry the cross-phase consistency pass pins in both Phase 1 and Phase 2) appear twice here.
        # Install each match key once; re-adding it would fail with ALREADY_EXISTS.
        seen: set = set()
        # Baseline: a table that already holds entries on the freshly-reset device carries the
        # program's compiled-in/static entries (the caller resets state before each install). A key
        # collision with those is EXPECTED and safe to skip; a collision on a table that was EMPTY
        # after reset would mean reset failed to clear a prior case's entry — a real bug we surface
        # rather than mask. get_table_entry_count is the fast software view (from_hw=False).
        installable = {e.table_name for e in entities
                       if not self.client.is_const_table(e.table_name)}
        prepopulated = {t for t in installable if self.client.get_table_entry_count(t) > 0}
        for ent in entities:
            # A const-entries table is immutable: its entries are compiled in and the control
            # plane cannot install into it. Skip (don't error) — its entries are already present.
            if self.client.is_const_table(ent.table_name):
                log.warning("skipping install into const-entries table %s (immutable)",
                            ent.table_name)
                continue
            sig = self._entry_match_sig(ent)
            if sig in seen:
                log.debug("skipping duplicate entry for table %s (same match key, phase %s)",
                          ent.table_name, ent.phase)
                continue
            seen.add(sig)
            try:
                if ent.is_default_entry:
                    # A default entry is a property of the table, not a row: BF-RT sets it with
                    # default_entry_set (data only) and entry_add would be the wrong call. This is
                    # the entry p4symbex emits for a table whose default action the controller
                    # installs (a keyless table, e.g. SwitchV2P's switch_config); without it the
                    # replay runs p4c's compiled-in default -- usually NoAction -- and silently
                    # diverges from the control plane the case was generated against.
                    self.client.set_default_entry(
                        table_name=ent.table_name,
                        datas=_to_client_data(ent),
                        action_name=ent.action_name,
                    )
                else:
                    self.client.insert_entity(
                        table_name=ent.table_name,
                        keys=_to_client_keys(ent),
                        datas=_to_client_data(ent),
                        action_name=ent.action_name,
                        priority=ent.priority,
                    )
            except Exception as ex:  # noqa: BLE001 - bfrt client raises a grpc-derived error type
                # `seen` dedupes entries repeated across phases in this txtpb, so a surviving
                # ALREADY_EXISTS is a collision with an entry already on the device. Skip it ONLY when
                # the table was pre-populated right after reset (a compiled-in/static entry we cannot
                # and need not re-add — used identically by the legit and attack runs, so the
                # differential comparison stays valid). If the table was EMPTY after reset, an
                # ALREADY_EXISTS means reset left stale state behind: fall through to `raise` so the
                # broken run surfaces instead of being silently masked. Mirrors bmv2 install_entities().
                if "ALREADY_EXISTS" in str(ex) and ent.table_name in prepopulated:
                    log.warning("entry collides with a pre-existing (compiled-in) entry in table %s; "
                                "skipping (idempotent install)", ent.table_name)
                    continue
                raise

    def _verify_entity_count(self, entities: List[BfRtEntity]) -> Tuple[bool, str]:
        """Read entry counts from the device and compare with the expected entity list.

        Const-entries tables are excluded: their entries are compiled in (not installed by us),
        so the device count would never match the txtpb-driven expectation."""
        from collections import Counter
        # Count UNIQUE match keys per table (identical cross-phase entries install once — see
        # _install_entities), so the expectation matches what the device actually holds.
        # Default entries are excluded as well: BF-RT models the default entry as a table property,
        # so entry_get never enumerates it and get_table_entry_count cannot see it. Counting it in
        # the expectation would make every case with one fail this check.
        unique_entries = {
            self._entry_match_sig(ent): ent for ent in entities
            if not ent.is_default_entry and not self.client.is_const_table(ent.table_name)}
        expected: Counter = Counter(ent.table_name for ent in unique_entries.values())
        for table_name, exp_count in expected.items():
            got = self.client.get_table_entry_count(table_name)
            if got != exp_count:
                return False, (f"table {table_name!r}: expected {exp_count} "
                               f"entries, device reports {got}")
        return True, ""

    def _send_only(self, phase: Phase, count: int, label: str,
                   inter: float = FLOOD_INTER) -> None:
        """Fast-inject a Phase-2 (tamper_only) packet `count` times with NO tcpdump capture.
        Tamper packets only advance on-device register state; their output is never used by the
        differential oracle (only Phase 1 / Phase 3 captures matter), so capturing them is pure
        overhead — prohibitive at repeat_count=k (e.g. 10001). The copies go out in one paced scapy
        call; the caller (_flood_register_to_target) re-sends the deficit to recover dropped packets."""
        in_iface = self.port_to_iface.get(phase.in_port)
        if in_iface is None:
            raise RuntimeError(f"unmapped input port {phase.in_port}")
        log.debug("Phase %s: injecting %dx on iface %s (port %d), inter=%s, no capture",
                  label, count, in_iface, phase.in_port, inter)
        _sudo_sendp(phase.in_packet, in_iface, count=count, inter=inter)

    def _read_register_max(self, reg_name: str) -> Optional[int]:
        """Returns the largest non-zero value across all cells of `reg_name`, or None on error.
        Used by the accumulation flood: we don't know the (hash-derived) bucket index, and bmv2 may
        touch several cells, so the max non-zero cell is the value the flood has driven the SO to."""
        try:
            cells, _cell_map = self.client.read_register_all(reg_name)
        except Exception as ex:  # noqa: BLE001
            log.warning("read_register_all(%s) failed: %s", reg_name, ex)
            return None
        best = 0
        for (_idx, cell) in cells:
            val = _coerce_register_value(cell)
            v = int.from_bytes(val, "big") if isinstance(val, (bytes, bytearray)) else int(val)
            if v > best:
                best = v
        return best

    def _flood_register_to_target(self, phase: Phase, count: int, reg_name: Optional[str],
                                  label: str) -> Optional[int]:
        """Drive an accumulating SO register to `count` despite packet loss on the (slow) model.
        Send `count` paced copies, then read the register's max non-zero cell and re-send the deficit.
        We don't know the (hash-derived) bucket index, so we track the max non-zero cell — for an
        incrementing accumulator this is monotone non-decreasing while the flood is landing packets.

        Stopping is ADAPTIVE rather than a fixed round count:
          - target reached (cur >= count): success;
          - PLATEAU: no net gain for FLOOD_STALL_ROUNDS consecutive rounds ⇒ packet loss is no longer
            recovering, resending is futile — stop (the differential then honestly reports no
            divergence);
          - REVERSAL: cur drops below the best value seen ⇒ the cell was reset or another writer is
            winning the race — stop (more sends won't help).
        A slow-but-progressing flood keeps going (it is no longer cut off at a fixed 40 rounds);
        FLOOD_MAX_ROUNDS is only a runaway backstop. Returns the achieved value (or None to skip)."""
        self._send_only(phase, count, label)
        if reg_name is None:
            return None
        best = self._read_register_max(reg_name)
        if best is None:
            return None
        if best >= count:
            return best
        stall = 0
        for rnd in range(FLOOD_MAX_ROUNDS):
            self._send_only(phase, count - best, f"{label}_topup{rnd}")
            cur = self._read_register_max(reg_name)
            if cur is None:
                return best
            log.debug("flood %s round %d: register max=%d / target=%d (best=%d, stall=%d/%d)",
                      reg_name, rnd, cur, count, best, stall, FLOOD_STALL_ROUNDS)
            if cur >= count:
                return cur
            if cur < best:
                log.warning("flood of %s reversed to %d (best %d, target %d); stopping "
                            "(cell reset / competing writer)", reg_name, cur, best, count)
                return best
            if cur == best:
                stall += 1
                if stall >= FLOOD_STALL_ROUNDS:
                    log.warning("flood of %s stalled at %d/%d (%d rounds no progress; packet loss "
                                "not recovering)", reg_name, best, count, stall)
                    return best
            else:
                best = cur
                stall = 0
        log.warning("flood of %s reached %d/%d after %d rounds (runaway backstop)",
                    reg_name, best, count, FLOOD_MAX_ROUNDS)
        return best

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

    # weak verification
    @staticmethod
    def _register_ok(got: bytes, reg: AffectedRegister) -> bool:
        """Does the value we read satisfy this case's declared success criterion?

        Compared as INTEGERS, never as raw bytes. _coerce_register_value returns the minimal-width
        encoding while the txtpb carries a fixed-width field, so `b'\x21'` and `b'\x00\x00\x00\x21'`
        are the same register value written two ways; a byte comparison tests the encoders rather
        than the switch and fails even when the tamper worked.
        """
        got_i = int.from_bytes(got, "big") if got else 0
        if reg.at_least:
            # Accumulating register: the flood stops as soon as it reaches its target and packet
            # loss moves where that lands, so no exact value is observable. What matters is that the
            # counter crossed the point at which the sink flips.
            return got_i >= int.from_bytes(reg.min_value, "big")
        return got_i == int.from_bytes(reg.attacker_value, "big")

    @staticmethod
    def _register_want(reg: AffectedRegister) -> str:
        if reg.at_least:
            return f">= {int.from_bytes(reg.min_value, 'big')}"
        return f"== {int.from_bytes(reg.attacker_value, 'big')}"

    def _verify_register(self, reg: AffectedRegister, strong_verify: bool = False) -> Tuple[bool, str]:
        log.debug("verifying register %s[%d] want %s",
                  reg.register_name, reg.index, self._register_want(reg))
        # 1. Read ONLY the index this case cares about (one targeted Read, not the whole array).
        data = self.client.read_register(reg.register_name, reg.index)
        if data is not None:
            got = _coerce_register_value(data)
            if self._register_ok(got, reg):
                return True, ""
            # The exact index holds a non-zero, non-matching value: tampering didn't take.
            # Under strong_verify, even a zero here is a failure (value must be at this index).
            if any(got) or strong_verify:
                return False, (f"register {reg.register_name}[{reg.index}] = "
                               f"{int.from_bytes(got, 'big') if got else 0}, "
                               f"want {self._register_want(reg)}")
            # Weak mode: index reads zero — the attacker value may have landed elsewhere.
            log.debug("register %s[%d] reads zero; scanning all cells (weak verify)",
                      reg.register_name, reg.index)
        else:
            log.debug("register %s[%d] single-index read returned nothing", reg.register_name, reg.index)
            if strong_verify:
                return False, (f"register {reg.register_name}[{reg.index}] not present "
                               f"(single-index read)")

        # 2. Weak verify only: the value may have been written to a different index. Now (and only
        #    now) read the whole array and look for a match anywhere.
        cells, _cell_map = self.client.read_register_all(reg.register_name)
        for (idx, cell) in cells:
            if self._register_ok(_coerce_register_value(cell), reg):
                log.debug("register %s[%d] satisfies %s (expected index %d)",
                          reg.register_name, idx, self._register_want(reg), reg.index)
                return True, ""
        return False, (f"register {reg.register_name}[{reg.index}] want {self._register_want(reg)}, "
                       f"not satisfied by any of {len(cells)} cells read")

    # Removed: `_verify_sink_miss`, which inferred "Phase 3 MISSed the sink table" from a
    # forwarding deviation against Phase 1. It had no callers -- the oracle is `compare_runs`,
    # which compares the legit and the attack Phase-3 outputs and never asks about hit/miss -- and
    # its premise is wrong for an action-divergence case, where the sink is reached in BOTH runs
    # and the deviation comes from a different action, not from a MISS.

    def replay(self, case: TamperCase, phase_indices: List[int], label: str,
               verify_regs_after: Optional[int] = None
               ) -> Tuple[Dict[int, List[bytes]], str]:
        """Install entities (+ multicast group) and send the given phases in order. Returns
        (last_phase_capture, reg_diag). State must already be reset by the caller; the differential
        comparison of two replays (legit vs attack) is the oracle. When `verify_regs_after` is the
        index of the phase that writes the attacker register (Phase 2 = 1), read the register right
        after that phase (before Phase 3 overwrites it) and return a human diagnostic."""
        self._install_multicast(case)
        try:
            self._install_entities(case.entities)
            last_cap: Dict[int, List[bytes]] = {}
            reg_diag = ""
            for i in phase_indices:
                phase = case.phases[i]
                reps = max(1, getattr(phase, "repeat_count", 1))
                if getattr(phase, "tamper_only", False):
                    # Phase-2 tamper packets: state-advancing only, output never compared. For a
                    # multi-packet accumulation (reps>1) drive the SO register to `reps`, re-sending
                    # the deficit to recover packets the model dropped; for a single tamper packet
                    # just inject it once.
                    if reps > 1:
                        reg_name = (case.affected_registers[0].register_name
                                    if case.affected_registers else None)
                        self._flood_register_to_target(phase, reps, reg_name, f"{label}_p{i + 1}")
                    else:
                        self._send_only(phase, 1, f"{label}_p{i + 1}")
                else:
                    # Phase 1 / Phase 3: capture for the differential. (repeat_count is 1 here.)
                    last_cap = self._send_and_capture(phase, f"{label}_p{i + 1}")
                if i == verify_regs_after and case.affected_registers:
                    reg_diag = self._diag_registers(case.affected_registers)
            return last_cap, reg_diag
        finally:
            self._remove_multicast(case)

    def _diag_registers(self, affected_registers: List[AffectedRegister]) -> str:
        """Confirm (weakly — scanning all cells) that the attacker value is present in each
        affected register after Phase 2. Diagnostic only; the oracle is the output differential."""
        parts = []
        for reg in affected_registers:
            try:
                # Honour --strong-verify. It reaches PacketTester.strong_verify via
                # tofino_driver.py, but this call site used to hardcode False, so the flag
                # had no effect on the Tofino register check.
                ok, why = self._verify_register(reg, strong_verify=self.strong_verify)
            except Exception as ex:  # noqa: BLE001
                ok, why = False, str(ex)
            tag = "written" if ok else f"NOT-written ({why})"
            parts.append(f"{reg.register_name}[{reg.index}]={reg.attacker_value.hex()}:{tag}")
        return "; ".join(parts)

    @staticmethod
    def _observed(capture: Dict[int, List[bytes]]) -> Optional[Tuple[int, bytes]]:
        """Reduce a capture {port: [pkts]} to the switch's output (port, first_packet_bytes),
        or None when the packet was dropped (no output on any monitored port)."""
        port = next((p for p, pkts in capture.items() if pkts), None)
        if port is None:
            return None
        return port, capture[port][0]

    def compare_runs(self, legit: Dict[int, List[bytes]],
                     attack: Dict[int, List[bytes]]) -> Tuple[bool, str]:
        """Differential oracle. `legit` = Phase-3 output of (Phase 1 -> Phase 3); `attack` =
        Phase-3 output of (Phase 1 -> Phase 2 -> Phase 3). Both share Phase 1, so any difference
        is caused solely by the attacker's Phase-2 write. Returns (vulnerable, reason): True/"OK"
        when the attacker changes the replayed output (a real tamper), False/"FAIL" when it has no
        observable effect (benign/masked corruption)."""
        o_legit = self._observed(legit)
        o_attack = self._observed(attack)
        if o_legit == o_attack:
            return False, ("no divergence: Phase-2 write has no observable effect on the replayed "
                           "Phase-3 output (benign / masked corruption)")

        def desc(o: Optional[Tuple[int, bytes]]) -> str:
            return "drop" if o is None else f"port {o[0]}, {len(o[1])}B"

        if (o_legit is None) != (o_attack is None):
            kind = "drop-state changed"
        elif o_legit is not None and o_attack is not None and o_legit[0] != o_attack[0]:
            kind = "output port changed"
        else:
            kind = f"packet bytes differ — {_pkt_diff(o_legit[1], o_attack[1])}"
        return True, (f"VULNERABLE: attacker's Phase-2 write changes the replayed Phase-3 output "
                      f"({kind}: legit={desc(o_legit)} vs attack={desc(o_attack)})")

    def _install_multicast(self, case: TamperCase) -> None:
        for mc in getattr(case, "multicast_groups", []):
            ports = mc.replica_ports or [0]
            try:
                self.client.install_multicast_group(mc.mgid, ports)
            except Exception as ex:
                log.warning("install_multicast_group(mgid=%s) failed: %s", mc.mgid, ex)

    def _remove_multicast(self, case: TamperCase) -> None:
        for mc in getattr(case, "multicast_groups", []):
            try:
                self.client.remove_multicast_group(mc.mgid)
            except Exception as ex:
                log.debug("remove_multicast_group(mgid=%s) failed: %s", mc.mgid, ex)



def _coerce_register_value(data) -> bytes:
    """Normalize whatever bfrt_grpc returns for a register read to bytes.

    BF-RT can return register data in two shapes:

    *Flat dict* (common with read_register_all):
        {'<table>.<field>': [<int_pipe0>, <int_pipe1>, ...],
         'action_name': None, 'is_default_entry': False}
    The value list contains one integer per pipeline; we take the first
    non-zero entry (pipe 0 after a write).

    *Legacy nested dict* (older bfrt_grpc data.to_dict()):
        {'fields': [{'name': '...', 'stream': b'...', ...}, ...]}
    """
    if isinstance(data, (bytes, bytearray)):
        return bytes(data)
    if isinstance(data, int):
        return data.to_bytes((data.bit_length() + 7) // 8 or 1, "big")
    if isinstance(data, dict):
        # --- flat dict format -------------------------------------------
        _META_KEYS = {"action_name", "is_default_entry", "fields"}
        for k, v in data.items():
            if k in _META_KEYS:
                continue
            # Per-pipe list: pick first non-zero value, fall back to pipe 0.
            if isinstance(v, list) and v:
                val = next((x for x in v if x), v[0])
                if isinstance(val, int):
                    return val.to_bytes((val.bit_length() + 7) // 8 or 1, "big")
                if isinstance(val, (bytes, bytearray)):
                    return bytes(val)
            if isinstance(v, int):
                return v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
            if isinstance(v, (bytes, bytearray)):
                return bytes(v)
        # --- legacy nested dict format ----------------------------------
        for fld in data.get("fields", []):
            v = fld.get("stream", fld.get("int_val"))
            if isinstance(v, (bytes, bytearray)):
                return bytes(v)
            if isinstance(v, int):
                return v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
    return b""


# Re-exported for convenience:
parse_case = parse_tampering_txtpb
