"""Minimal BF-Runtime gRPC client wrapper, sibling of ``bmv2/p4_client.py``.

This module wraps the Intel BF-SDE Python library ``bfrt_grpc.client`` (often
imported as ``gc``) so that the higher-level Tofino tampering harness can talk
to a live ``tofino_model`` (via ``bf_switchd``) over BF-Runtime gRPC.

The shape is intentionally parallel to ``bmv2/p4_client.py.P4RuntimeClient``:
construct with an address, then call ``insert_entity`` / ``write_register`` /
``read_register_all`` / ``clear_table`` etc.

**Runtime requirement:** the ``bfrt_grpc`` Python package is part of the Intel
BF-SDE and is not pip-installable. This module imports it lazily so that the
harness's `--stage build` path remains usable on a machine without the SDE; the
import only fires when a `BfRtClient` instance is actually constructed.
"""

from __future__ import annotations

import glob
import logging
import os
import sys
from dataclasses import dataclass
from typing import Any, Iterable, List, Optional, Tuple

log = logging.getLogger("bfrt_grpc_client")


# Sentinel object — `bfrt_grpc` is loaded lazily inside the BfRtClient
# constructor so that import-time failures don't break the build/generate
# pipeline on hosts without the BF-SDE.
_gc: Any = None
_bfruntime_pb2: Any = None


def _lazy_import_bfrt() -> None:
    global _gc, _bfruntime_pb2
    if _gc is not None:
        return
    # bfrt_grpc lives at $SDE_INSTALL/lib/python*/site-packages/tofino/bfrt_grpc/
    # but client.py itself imports 'bfrt_grpc.*' (not 'tofino.bfrt_grpc.*'), so
    # we need both the site-packages root (for grpc and other deps) AND the
    # tofino/ sub-directory (so bfrt_grpc is importable as a top-level package).
    sde_install = os.environ.get("SDE_INSTALL", "")
    if sde_install:
        for sp in glob.glob(f"{sde_install}/lib/python*/site-packages"):
            if sp not in sys.path:
                sys.path.insert(0, sp)
            tofino_sp = f"{sp}/tofino"
            if tofino_sp not in sys.path:
                sys.path.insert(0, tofino_sp)
    try:
        import bfrt_grpc.client as gc  # type: ignore[import-not-found]
        import bfrt_grpc.bfruntime_pb2 as bfruntime_pb2  # type: ignore[import-not-found]
    except ImportError as e:
        raise ImportError(
            "bfrt_grpc is not installed on this host. It ships with the Intel "
            "BF-SDE; set $SDE_INSTALL or install/source the SDE environment "
            "before running the Tofino test stage."
        ) from e
    _gc = gc
    _bfruntime_pb2 = bfruntime_pb2


@dataclass
class KeyField:
    """One element of a BfRt table key. ``match_kind`` is one of
    ``"exact"``, ``"ternary"``, ``"lpm"``, ``"range"``, ``"optional"``."""
    field_name: str
    match_kind: str
    # `value` is always present; the second element holds the secondary value
    # (mask, prefix_len, or high-end-of-range) when match_kind requires it.
    value: bytes
    secondary: Optional[Any] = None


@dataclass
class DataField:
    """One element of a BfRt table data (action param or register value)."""
    field_name: str
    value: bytes


class BfRtClient:
    """Wraps ``bfrt_grpc.client.ClientInterface`` for the test harness.

    Parameters mirror ``bmv2/p4_client.P4RuntimeClient`` where possible.
    """

    def __init__(self, grpc_address: str = "127.0.0.1:50052", device_id: int = 0,
                 client_id: int = 0, p4_name: Optional[str] = None,
                 pipe_id: int = 0xFFFF) -> None:
        _lazy_import_bfrt()
        self.device_id = device_id
        self.client_id = client_id
        self.p4_name = p4_name
        # `gc.ClientInterface(grpc_addr, client_id, device_id, num_tries=...)`
        self.interface = _gc.ClientInterface(grpc_address, client_id=client_id,
                                             device_id=device_id, num_tries=5)
        self.target = _gc.Target(device_id=device_id, pipe_id=pipe_id)
        # `bfrt_info` is populated after `bind_pipeline(p4_name)`.
        self.bfrt_info = None
        # Cache of table_name -> is-const (a `const entries` table is immutable: the control
        # plane cannot add/modify/delete its entries). Populated lazily by is_const_table().
        self._const_tables: Dict[str, bool] = {}
        if p4_name is not None:
            self.bind_pipeline(p4_name)

    # --- pipeline management ------------------------------------------------
    def set_forwarding_pipeline_config(self, conf_path: str) -> None:
        """Push the device's .conf file at `conf_path` to bf_switchd."""
        # The Intel BF-SDE driver normally launches with the .conf already
        # loaded via `--install-dir ...`; the gRPC SetForwardingPipelineConfig
        # RPC is the runtime alternative when bringing up programs dynamically.
        # Implementation is intentionally left for the test-stage milestone —
        # most labs preload the pipeline at switchd startup.
        raise NotImplementedError(
            "set_forwarding_pipeline_config is a runtime-only path; for now, "
            "start bf_switchd with the desired program at boot.")

    def bind_pipeline(self, p4_name: str) -> None:
        """Bind client to the named program and fetch its schema.

        bind_pipeline_config() sends SetForwardingPipelineConfig(BIND) to
        bf_switchd, which authorises this client to issue Write RPCs for the
        named program. bfrt_info_get() then fetches the schema (bf-rt.json)
        so table/field names can be resolved locally. Both calls are required:
        skipping bind_pipeline_config causes Write RPCs to fail with a
        generic UNKNOWN status that has no structured error details
        ("No binary details field").
        """
        self.p4_name = p4_name
        self.interface.bind_pipeline_config(p4_name)
        self.bfrt_info = self.interface.bfrt_info_get(p4_name)

    # --- table operations ---------------------------------------------------
    def insert_entity(self, table_name: str, keys: Iterable[KeyField],
                      datas: Iterable[DataField],
                      action_name: Optional[str] = None,
                      priority: int = 0) -> None:
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        table = self.bfrt_info.table_get(table_name)
        key_tuples = [self._key_to_tuple(k) for k in keys]
        if priority:
            key_tuples.append(_gc.KeyTuple("$MATCH_PRIORITY", priority))
        data_tuples = [_gc.DataTuple(d.field_name, d.value) for d in datas]
        key_list = [table.make_key(key_tuples)]
        data_list = [table.make_data(data_tuples, action_name)]
        table.entry_add(self.target, key_list, data_list)

    def set_default_entry(self, table_name: str, datas: Iterable[DataField],
                          action_name: Optional[str] = None) -> None:
        """Set a table's DEFAULT entry -- the action a controller installs on a keyless table.

        Data only, no key: BF-RT models the default entry as a property of the table rather than as
        a row, so there is nothing to `make_key` and `entry_add` is the wrong call. Mirrors the
        `overrideDefaultEntry` helper p4symbex already emits into its PTF tests.
        """
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        table = self.bfrt_info.table_get(table_name)
        data_tuples = [_gc.DataTuple(d.field_name, d.value) for d in datas]
        table.default_entry_set(self.target, table.make_data(data_tuples, action_name))

    def get_table_entry_count(self, table_name: str) -> int:
        """Return the number of entries currently in the named table (software view)."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        try:
            table = self.bfrt_info.table_get(table_name)
        except Exception as e:
            log.debug("table_get(%s) failed: %s", table_name, e)
            return 0
        count = 0
        try:
            for _data, _key in table.entry_get(self.target, [], {"from_hw": False}):
                count += 1
        except Exception as e:
            log.debug("get_table_entry_count(%s) entry_get failed: %s", table_name, e)
        return count

    def is_const_table(self, table_name: str) -> bool:
        """Return True if `table_name` is a `const entries` table (immutable from the control
        plane). p4c marks such tables with the BfRt ``ConstTable`` attribute in bf-rt.json
        (see p4c control-plane/bfruntime.cpp). The control plane cannot add/modify/delete their
        entries, so the harness must not try to install into or clear them.

        Best-effort: probes the table's BfRt attribute/annotation metadata across BF-SDE API
        shapes and caches the result. Returns False if constness cannot be determined (safe:
        preserves the prior behaviour of attempting and tolerating failures)."""
        if table_name in self._const_tables:
            return self._const_tables[table_name]
        const = False
        try:
            table = self.bfrt_info.table_get(table_name)
            info = getattr(table, "info", None)
            candidates = []
            for obj in (info, table):
                if obj is None:
                    continue
                for accessor in ("get_attributes", "attributes_get", "attributes",
                                 "get_annotations", "annotations"):
                    attr = getattr(obj, accessor, None)
                    try:
                        candidates.append(attr() if callable(attr) else attr)
                    except Exception:  # noqa: BLE001 - accessor may need args; ignore
                        continue
            for c in candidates:
                if c and "consttable" in str(c).replace(" ", "").replace("_", "").lower():
                    const = True
                    break
        except Exception as e:  # noqa: BLE001
            log.debug("is_const_table(%s): could not determine constness: %s", table_name, e)
        self._const_tables[table_name] = const
        if const:
            log.debug("table %s is a const-entries table (immutable); skipping mutations",
                      table_name)
        return const

    def clear_table(self, table_name: str) -> None:
        """Delete every entry in the named table by reading it back first and
        issuing per-entry deletes. Wildcard delete semantics vary across BF-SDE
        versions; iterating is the safe option for the test harness."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        # A const-entries table is immutable; its entries cannot be deleted. Skip it.
        if self.is_const_table(table_name):
            return
        try:
            table = self.bfrt_info.table_get(table_name)
        except Exception as e:
            log.debug("table_get(%s) failed: %s", table_name, e)
            return
        try:
            keys_to_delete = []
            for _data, key in table.entry_get(self.target, [], {"from_hw": False}):
                keys_to_delete.append(key)
            for key in keys_to_delete:
                try:
                    table.entry_del(self.target, [key])
                except Exception as e:
                    log.debug("entry_del(%s) failed: %s", table_name, e)
        except Exception as e:
            log.debug("clear_table(%s) entry_get failed: %s", table_name, e)
        # entry_get does not enumerate the default entry and entry_del cannot remove it, so a
        # default entry set for one case would otherwise survive into the next and silently change
        # its control plane. Reset it back to the compiled-in default explicitly.
        try:
            table.default_entry_reset(self.target)
        except Exception as e:
            log.debug("default_entry_reset(%s) failed (ok if unsupported): %s", table_name, e)

    # Table types that support entry_del; all others (Register, DynHash*, etc.)
    # either have no user-deletable entries or don't support the operation.
    _CLEARABLE_TABLE_TYPES = frozenset({
        "MatchAction_Direct",
        "MatchAction_Indirect",
        "ActionProfile",
        "ActionSelector",
    })

    def clear_all_tables(self) -> None:
        """Iterate every non-fixed match/action table and clear it.

        Skips built-in ``$…`` tables, idle-timeout shadows, and any table
        whose BFRT type is not in ``_CLEARABLE_TABLE_TYPES`` (e.g. Register,
        DynHash* tables do not support entry_del).
        """
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        for name, table in self.bfrt_info.table_dict.items():
            if name.startswith("$") or name.endswith(".idle"):
                continue
            try:
                table_type = table.info.type_get()
            except Exception:
                continue
            if table_type not in self._CLEARABLE_TABLE_TYPES:
                continue
            self.clear_table(name)

    # --- multicast (packet replication engine) ------------------------------
    # p4symbex models a multicast-forwarded packet as a single representative egress port.
    # For the replayed packet to actually egress that port, the control plane must install the
    # multicast group it carries (standard BfRt fixed tables $pre.node and $pre.mgid). These
    # helpers install/remove a minimal group: one L1 node fanning out to `ports`, attached to
    # `mgid`. NOTE: exercised only on a live Tofino target (not available on this dev machine).
    def install_multicast_group(self, mgid: int, ports: Iterable[int],
                                node_id: Optional[int] = None, rid: int = 0) -> None:
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        if node_id is None:
            node_id = mgid
        port_list = [int(p) for p in ports]
        # $pre.node: define the L1 node and its member port set.
        node = self.bfrt_info.table_get("$pre.node")
        node_key = node.make_key([_gc.KeyTuple("$MULTICAST_NODE_ID", node_id)])
        node_data = node.make_data([
            _gc.DataTuple("$MULTICAST_RID", rid),
            _gc.DataTuple("$DEV_PORT", int_arr_val=port_list),
        ])
        node.entry_add(self.target, [node_key], [node_data])
        # $pre.mgid: attach the node to the multicast group id.
        mgid_tbl = self.bfrt_info.table_get("$pre.mgid")
        mgid_key = mgid_tbl.make_key([_gc.KeyTuple("$MGID", mgid)])
        mgid_data = mgid_tbl.make_data([
            _gc.DataTuple("$MULTICAST_NODE_ID", int_arr_val=[node_id]),
            _gc.DataTuple("$MULTICAST_NODE_L1_XID_VALID", bool_arr_val=[False]),
            _gc.DataTuple("$MULTICAST_NODE_L1_XID", int_arr_val=[0]),
        ])
        mgid_tbl.entry_add(self.target, [mgid_key], [mgid_data])

    def remove_multicast_group(self, mgid: int, node_id: Optional[int] = None) -> None:
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        if node_id is None:
            node_id = mgid
        # Remove in reverse dependency order: detach the group, then delete the node.
        try:
            mgid_tbl = self.bfrt_info.table_get("$pre.mgid")
            mgid_tbl.entry_del(self.target,
                               [mgid_tbl.make_key([_gc.KeyTuple("$MGID", mgid)])])
        except Exception as e:
            log.debug("remove_multicast_group: $pre.mgid del(%s) failed: %s", mgid, e)
        try:
            node = self.bfrt_info.table_get("$pre.node")
            node.entry_del(self.target,
                           [node.make_key([_gc.KeyTuple("$MULTICAST_NODE_ID", node_id)])])
        except Exception as e:
            log.debug("remove_multicast_group: $pre.node del(%s) failed: %s", node_id, e)

    # --- register operations ------------------------------------------------
    def write_register(self, register_name: str, index: int, value: Any) -> None:
        """Set register[index] = value. `value` may be an int (single-field
        registers) or a list of (field_name, value) tuples for struct registers.
        """
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        reg = self.bfrt_info.table_get(register_name)
        key_list = [reg.make_key([_gc.KeyTuple("$REGISTER_INDEX", index)])]
        if isinstance(value, list):
            data_tuples = [_gc.DataTuple(fname, fval) for fname, fval in value]
        else:
            data_tuples = [_gc.DataTuple("f1", value)]
        reg.entry_add(self.target, key_list, [reg.make_data(data_tuples)])

    def read_register(self, register_name: str, index: int) -> Optional[Any]:
        """Read a single register cell by index (one targeted Read, not the whole array).

        Returns the cell's data dict (same shape as read_register_all's values), or None if the
        index returned no entry. Use this to check the one index a test cares about before
        falling back to read_register_all()."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        reg = self.bfrt_info.table_get(register_name)
        key = reg.make_key([_gc.KeyTuple("$REGISTER_INDEX", index)])
        try:
            for data, _key in reg.entry_get(self.target, [key], {"from_hw": True}):
                return data.to_dict()
        except Exception as e:  # noqa: BLE001 - index out of range / read error → caller falls back
            log.debug("read_register(%s[%d]) failed: %s", register_name, index, e)
        return None

    def read_register_all(self, register_name: str) -> List[Tuple[int, Any]]:
        """Return [(index, value), ...] for every cell of the named register."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        reg = self.bfrt_info.table_get(register_name)
        out: List[Tuple[int, Any]] = []
        out_map: Dict[int, Any] = {}
        # Query with an empty key set = wildcard read.
        for data, key in reg.entry_get(self.target, [], {"from_hw": True}):
            idx = key.to_dict().get("$REGISTER_INDEX", {}).get("value")
            out.append((idx, data.to_dict()))
            out_map[idx] = data.to_dict()
        return out, out_map

    def clear_all_registers(self, register_name: str = "") -> None:
        """Reset all cells of the specified register (or every register) to zero.

        Registers are fixed-size arrays — entries cannot be deleted and
        ``entry_rst()`` is absent in some SDE versions.  The portable approach
        is a bulk ``entry_mod`` that writes 0 to every cell index in one call.
        """
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        if register_name:
            regs = [register_name]
        else:
            regs = []
            for name, table in self.bfrt_info.table_dict.items():
                if name.startswith("$"):
                    continue
                try:
                    if table.info.type_get() == "Register":
                        regs.append(name)
                except Exception:
                    continue
        log.debug("clear_all_registers: resetting %d registers", len(regs))
        for i, reg_name in enumerate(regs):
            log.debug(f"[{i+1}/{len(regs)}] {reg_name}")
            reg = self.bfrt_info.table_get(reg_name)
            try:
                size = reg.info.size_get()
                # Pick the first non-$ data field — that is the register's value field.
                field_name = next(
                    (f for f in reg.info.data_field_name_list_get()
                     if not f.startswith("$")),
                    "f1",
                )
                probe_key = reg.make_key([_gc.KeyTuple("$REGISTER_INDEX", 0)])
                probe_data = reg.make_data([_gc.DataTuple(field_name, 0)])
                try:
                    reg.entry_mod(self.target, [probe_key], [probe_data])
                except Exception as probe_err:
                    err_str = str(probe_err)
                    if "NOT_FOUND" in err_str or "Table not found" in err_str:
                        log.debug("skip register %s: not present at runtime", reg_name)
                        continue
                    raise
                self._reset_register_cells(reg, field_name, size)
                log.debug("reset register %s (%d cells)", reg_name, size)
            except Exception as e:
                log.warning("clear_all_registers(%s) failed: %s", reg_name, e)

    # Max number of register cells to put in a single Write RPC when falling back to
    # per-cell modify. A wide register array (e.g. a bit<1> flag indexed 0..65535) would
    # otherwise be one giant WriteRequest that bursts the control plane / exceeds the gRPC
    # message limit; chunking keeps each RPC bounded.
    _REGISTER_MOD_CHUNK = 1024

    def _reset_register_cells(self, reg: Any, field_name: str, size: int) -> None:
        """Reset every cell of register `reg` to 0 without bursting the control plane.

        Index 0 has already been reset by the caller's probe. Strategy, best→fallback:
          1. ``reg.clear(target)`` — BfRt's bulk server-side reset: one RPC for the whole
             array, no per-cell traffic at all.
          2. Otherwise a *chunked* ``entry_mod``: a single ``entry_mod`` call already packs
             its whole key/data list into one Write RPC (not one-per-cell), so chunking the
             index range bounds each RPC's size for very wide arrays (e.g. a bit<1> flag
             indexed 0..65535) instead of emitting one oversized request.
        """
        # 1) Bulk clear — the cheapest path when the SDE exposes it for Register tables.
        clear_fn = getattr(reg, "clear", None)
        if callable(clear_fn):
            try:
                clear_fn(self.target)
                return
            except Exception as e:  # noqa: BLE001 - older SDEs may not support clear() on registers
                log.debug("reg.clear() unsupported/failed, falling back to chunked mod: %s", e)

        log.debug("reg.clear() not supported; resetting cells 1..%d with chunked entry_mod", size - 1)

        # 2) Chunked entry_mod over indices 1..size-1; each chunk is one batched Write RPC.
        for lo in range(1, size, self._REGISTER_MOD_CHUNK):
            hi = min(lo + self._REGISTER_MOD_CHUNK, size)
            key_list = [reg.make_key([_gc.KeyTuple("$REGISTER_INDEX", i)])
                        for i in range(lo, hi)]
            data_list = [reg.make_data([_gc.DataTuple(field_name, 0)])
                         for _ in range(lo, hi)]
            reg.entry_mod(self.target, key_list, data_list)

    # --- helpers ------------------------------------------------------------
    def _key_to_tuple(self, k: KeyField) -> Any:
        if k.match_kind == "exact":
            return _gc.KeyTuple(k.field_name, k.value)
        if k.match_kind == "ternary":
            return _gc.KeyTuple(k.field_name, k.value, mask=k.secondary)
        if k.match_kind == "lpm":
            return _gc.KeyTuple(k.field_name, k.value, prefix_len=k.secondary)
        if k.match_kind == "range":
            return _gc.KeyTuple(k.field_name, low=k.value, high=k.secondary)
        if k.match_kind == "optional":
            return _gc.KeyTuple(k.field_name, k.value)
        raise ValueError(f"unknown match kind: {k.match_kind!r}")

    def close(self) -> None:
        """Release the BF-Runtime gRPC connection. ClientInterface's cleanup method varies by
        SDE version (``tear_down`` is absent in some — e.g. open-p4studio), so try the known
        ones, then close the underlying gRPC channel directly so the connection to bf_switchd is
        actually dropped (a lingering stream can keep the server side busy across a restart)."""
        for meth in ("tear_down", "_tear_down_stream", "stop"):
            fn = getattr(self.interface, meth, None)
            if callable(fn):
                try:
                    fn()
                    break
                except Exception as e:
                    log.debug("BfRtClient.close %s(): %s", meth, e)
        for attr in ("channel", "_channel"):
            ch = getattr(self.interface, attr, None)
            if ch is not None and hasattr(ch, "close"):
                try:
                    ch.close()
                except Exception as e:
                    log.debug("BfRtClient.close channel(%s): %s", attr, e)
