# Copyright 2021-2023 H2020 TeraFlow (https://www.teraflow-h2020.eu/)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Minimal P4Runtime gRPC client.

Trimmed from Theseus/examples/p4_client.py for use by tools/p4csd. Keeps only
the surface area the tampering harness needs: handshake/arbitration, pipeline
config, table-entry insert/delete, wildcard read, and teardown.
"""

import enum
import logging
import queue
import sys
import threading
from functools import wraps

import grpc
import google.protobuf.text_format
from google.rpc import code_pb2, status_pb2

from p4.v1 import p4runtime_pb2, p4runtime_pb2_grpc


STREAM_ATTR_ARBITRATION = "arbitration"
STREAM_ATTR_PACKET = "packet"
STREAM_ATTR_DIGEST = "digest"
STREAM_ATTR_IDLE_NOT = "idle_timeout_notification"
STREAM_ATTR_UNKNOWN = "unknown"

LOGGER = logging.getLogger(__name__)


class P4RuntimeErrorFormatException(Exception):
    """Raised when a gRPC error's trailing-metadata details cannot be parsed."""


class P4RuntimeErrorIterator:
    """Iterate over the p4.Error entries inside a gRPC error Status object."""

    def __init__(self, grpc_error):
        assert grpc_error.code() == grpc.StatusCode.UNKNOWN
        self.grpc_error = grpc_error

        error = None
        for meta in self.grpc_error.trailing_metadata():
            if meta[0] == "grpc-status-details-bin":
                error = status_pb2.Status()
                error.ParseFromString(meta[1])
                break
        if error is None:
            raise P4RuntimeErrorFormatException("No binary details field")
        if len(error.details) == 0:
            raise P4RuntimeErrorFormatException(
                "Binary details field has empty Any details repeated field")
        self.errors = error.details
        self.idx = 0

    def __iter__(self):
        return self

    def __next__(self):
        while self.idx < len(self.errors):
            p4_error = p4runtime_pb2.Error()
            one_error_any = self.errors[self.idx]
            if not one_error_any.Unpack(p4_error):
                raise P4RuntimeErrorFormatException(
                    "Cannot convert Any message to p4.Error")
            if p4_error.canonical_code == code_pb2.OK:
                continue
            val = self.idx, p4_error
            self.idx += 1
            return val
        raise StopIteration


class P4RuntimeWriteException(Exception):
    """Aggregates per-update errors from a failed P4Runtime Write RPC."""

    def __init__(self, grpc_error):
        assert grpc_error.code() == grpc.StatusCode.UNKNOWN
        super().__init__()
        self.errors = []
        try:
            for error_tuple in P4RuntimeErrorIterator(grpc_error):
                self.errors.append(error_tuple)
        except P4RuntimeErrorFormatException as ex:
            raise P4RuntimeException(grpc_error) from ex

    def __str__(self):
        message = "Error(s) during Write:\n"
        for idx, p4_error in self.errors:
            code_name = code_pb2._CODE.values_by_number[
                p4_error.canonical_code].name
            message += f"\t* At index {idx}: {code_name}, '{p4_error.message}'\n"
        return message


class P4RuntimeException(Exception):
    """Wraps a non-Write gRPC error."""

    def __init__(self, grpc_error):
        super().__init__()
        self.grpc_error = grpc_error

    def __str__(self):
        return (f"P4Runtime RPC error ({self.grpc_error.code().name}): "
                f"{self.grpc_error.details()}")


def parse_p4runtime_write_error(func):
    @wraps(func)
    def handle(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except grpc.RpcError as ex:
            if ex.code() != grpc.StatusCode.UNKNOWN:
                raise
            raise P4RuntimeWriteException(ex) from None
    return handle


def parse_p4runtime_error(func):
    @wraps(func)
    def handle(*args, **kwargs):
        try:
            return func(*args, **kwargs)
        except grpc.RpcError as ex:
            raise P4RuntimeException(ex) from None
    return handle


@enum.unique
class WriteOperation(enum.Enum):
    insert = 1
    update = 2
    delete = 3


_WRITE_OP_MAP = {
    WriteOperation.insert: p4runtime_pb2.Update.INSERT,
    WriteOperation.update: p4runtime_pb2.Update.MODIFY,
    WriteOperation.delete: p4runtime_pb2.Update.DELETE,
}


def _attach_entity(entity, update):
    """Copy `entity` into `update.entity.<oneof_field>`.

    Accepts either a top-level p4runtime Entity or one of the inner messages
    that the harness uses (TableEntry, ActionProfileGroup, ActionProfileMember).
    """
    if isinstance(entity, p4runtime_pb2.Entity):
        update.entity.CopyFrom(entity)
        return True
    if isinstance(entity, p4runtime_pb2.TableEntry):
        update.entity.table_entry.CopyFrom(entity)
        return True
    if isinstance(entity, p4runtime_pb2.ActionProfileGroup):
        update.entity.action_profile_group.CopyFrom(entity)
        return True
    if isinstance(entity, p4runtime_pb2.ActionProfileMember):
        update.entity.action_profile_member.CopyFrom(entity)
        return True
    if isinstance(entity, p4runtime_pb2.RegisterEntry):
        update.entity.register_entry.CopyFrom(entity)
        return True
    return False


class P4RuntimeClient:
    """Minimal P4Runtime client.

    Parameters
    ----------
    device_id : int
        P4 device id (BMv2 simple_switch_grpc defaults to 0).
    grpc_address : str
        host:port of the P4Runtime server.
    election_id : tuple[int, int]
        (high, low) mastership election id; must beat other clients to be primary.
    role_name : str, optional
        Role name; left unset for single-role setups.
    """

    def __init__(self, device_id, grpc_address, election_id, role_name=None):
        self.device_id = device_id
        self.election_id = election_id
        self.role_name = role_name
        LOGGER.debug("Connecting to device %d at %s", device_id, grpc_address)

        self.channel = grpc.insecure_channel(grpc_address)
        self.stub = p4runtime_pb2_grpc.P4RuntimeStub(self.channel)

        self.stream_in_q = None
        self.stream_out_q = None
        self.stream = None
        self.stream_recv_thread = None

        try:
            self._set_up_stream()
        except P4RuntimeException:
            LOGGER.critical("Failed to connect to P4Runtime server")
            sys.exit(1)
        LOGGER.info("P4Runtime client is successfully invoked")

    def _set_up_stream(self):
        self.stream_out_q = queue.Queue()
        self.stream_in_q = {
            STREAM_ATTR_ARBITRATION: queue.Queue(),
            STREAM_ATTR_PACKET: queue.Queue(),
            STREAM_ATTR_DIGEST: queue.Queue(),
            STREAM_ATTR_IDLE_NOT: queue.Queue(),
            STREAM_ATTR_UNKNOWN: queue.Queue(),
        }

        def stream_req_iterator():
            while True:
                stream_p = self.stream_out_q.get()
                if stream_p is None:
                    break
                yield stream_p

        def stream_recv_wrapper(stream):
            @parse_p4runtime_error
            def stream_recv():
                for stream_p in stream:
                    if stream_p.HasField("arbitration"):
                        self.stream_in_q[STREAM_ATTR_ARBITRATION].put(stream_p)
                    elif stream_p.HasField("packet"):
                        self.stream_in_q[STREAM_ATTR_PACKET].put(stream_p)
                    elif stream_p.HasField("digest"):
                        self.stream_in_q[STREAM_ATTR_DIGEST].put(stream_p)
                    else:
                        self.stream_in_q[STREAM_ATTR_UNKNOWN].put(stream_p)

            try:
                stream_recv()
            except P4RuntimeException as ex:
                LOGGER.critical("StreamChannel error, closing stream: %s", ex)
                for k in self.stream_in_q:
                    self.stream_in_q[k].put(None)

        self.stream = self.stub.StreamChannel(stream_req_iterator())
        self.stream_recv_thread = threading.Thread(
            target=stream_recv_wrapper, args=(self.stream,), daemon=True)
        self.stream_recv_thread.start()
        self._handshake()

    def _handshake(self):
        req = p4runtime_pb2.StreamMessageRequest()
        arbitration = req.arbitration
        arbitration.device_id = self.device_id
        election_id = arbitration.election_id
        election_id.high = self.election_id[0]
        election_id.low = self.election_id[1]
        if self.role_name is not None:
            arbitration.role.name = self.role_name
        self.stream_out_q.put(req)

        rep = self._get_stream_packet(STREAM_ATTR_ARBITRATION, timeout=2)
        if rep is None:
            LOGGER.critical("Failed to establish session with server")
            sys.exit(1)
        is_primary = (rep.arbitration.status.code == code_pb2.OK)
        LOGGER.debug("Session established, client is '%s'",
                     "primary" if is_primary else "backup")
        if not is_primary:
            LOGGER.warning("You are not the primary client; read-only access")

    def _get_stream_packet(self, type_, timeout=1):
        if type_ not in self.stream_in_q:
            LOGGER.warning("Unknown stream type %s", type_)
            return None
        try:
            return self.stream_in_q[type_].get(timeout=timeout)
        except queue.Empty:
            return None

    @parse_p4runtime_error
    def get_p4info(self):
        """Return the P4Info from the server (raw protobuf)."""
        req = p4runtime_pb2.GetForwardingPipelineConfigRequest()
        req.device_id = self.device_id
        req.response_type = \
            p4runtime_pb2.GetForwardingPipelineConfigRequest.P4INFO_AND_COOKIE
        rep = self.stub.GetForwardingPipelineConfig(req)
        return rep.config.p4info

    @parse_p4runtime_error
    def set_fwd_pipe_config(self, p4info_path, bin_path):
        """Install the forwarding pipeline (P4Info + bmv2 JSON)."""
        req = p4runtime_pb2.SetForwardingPipelineConfigRequest()
        req.device_id = self.device_id
        if self.role_name is not None:
            req.role = self.role_name
        election_id = req.election_id
        election_id.high = self.election_id[0]
        election_id.low = self.election_id[1]
        req.action = \
            p4runtime_pb2.SetForwardingPipelineConfigRequest.VERIFY_AND_COMMIT
        with open(p4info_path, "r", encoding="utf-8") as f_info, \
                open(bin_path, "rb") as f_bin:
            try:
                google.protobuf.text_format.Merge(f_info.read(), req.config.p4info)
            except google.protobuf.text_format.ParseError:
                LOGGER.error("Error when parsing P4Info")
                raise
            req.config.p4_device_config = f_bin.read()
        return self.stub.SetForwardingPipelineConfig(req)

    def tear_down(self):
        """Close the StreamChannel and the gRPC channel."""
        if self.stream_out_q:
            self.stream_out_q.put(None)
        if self.stream_in_q:
            for k in self.stream_in_q:
                self.stream_in_q[k].put(None)
        if self.stream_recv_thread:
            self.stream_recv_thread.join(timeout=2)
        self.channel.close()
        del self.channel

    def _new_write_request(self):
        req = p4runtime_pb2.WriteRequest()
        req.device_id = self.device_id
        if self.role_name is not None:
            req.role = self.role_name
        election_id = req.election_id
        election_id.high = self.election_id[0]
        election_id.low = self.election_id[1]
        return req

    @parse_p4runtime_write_error
    def _simple_write(self, req):
        try:
            return self.stub.Write(req)
        except grpc.RpcError as ex:
            if ex.code() != grpc.StatusCode.UNKNOWN:
                raise
            raise P4RuntimeWriteException(ex) from ex

    @parse_p4runtime_write_error
    def _write(self, entity, mode=WriteOperation.insert):
        if isinstance(entity, (list, tuple)):
            for ent in entity:
                self._write(ent, mode)
            return
        req = self._new_write_request()
        update = req.updates.add()
        update.type = _WRITE_OP_MAP[mode]
        if not _attach_entity(entity, update):
            type_name = type(entity).__name__
            raise P4RuntimeWriteException(
                f"{mode.name} operation for entity {type_name} not supported")
        self._simple_write(req)

    def insert(self, entity):
        """Insert a TableEntry / ActionProfile* / Entity."""
        return self._write(entity, WriteOperation.insert)

    def update(self, entity):
        return self._write(entity, WriteOperation.update)

    def delete(self, entity):
        return self._write(entity, WriteOperation.delete)

    @parse_p4runtime_error
    def read_one(self, entity):
        """Issue a wildcard Read for one Entity; returns a streaming iterator."""
        req = p4runtime_pb2.ReadRequest()
        if self.role_name is not None:
            req.role = self.role_name
        req.device_id = self.device_id
        req.entities.extend([entity])
        return self.stub.Read(req)
