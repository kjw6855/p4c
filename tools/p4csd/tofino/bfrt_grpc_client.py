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

    def clear_table(self, table_name: str) -> None:
        """Delete every entry in the named table by reading it back first and
        issuing per-entry deletes. Wildcard delete semantics vary across BF-SDE
        versions; iterating is the safe option for the test harness."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
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

    def read_register_all(self, register_name: str) -> List[Tuple[int, Any]]:
        """Return [(index, value), ...] for every cell of the named register."""
        assert self.bfrt_info is not None, "call bind_pipeline() first"
        reg = self.bfrt_info.table_get(register_name)
        out: List[Tuple[int, Any]] = []
        # Query with an empty key set = wildcard read.
        for data, key in reg.entry_get(self.target, [], {"from_hw": True}):
            idx = next((k.val for k in key.to_dict().get("fields", [])
                        if k.name == "$REGISTER_INDEX"), None)
            out.append((idx, data.to_dict()))
        return out

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
        try:
            self.interface.tear_down()
        except Exception as e:
            log.debug("BfRtClient.close: %s", e)
