"""Minimal BMv2 Thrift client for register operations.

BMv2's simple_switch_grpc exposes a Thrift management interface (default port
9090) alongside the P4Runtime gRPC interface.  P4Runtime register read/write
is not yet implemented in BMv2 (p4lang/PI#376), so register operations go
through this Thrift interface instead.
"""

import logging
from typing import List

LOGGER = logging.getLogger(__name__)

try:
    from thrift.transport import TSocket, TTransport
    from thrift.protocol import TBinaryProtocol, TMultiplexedProtocol
    from bm_runtime.standard import Standard
    _THRIFT_AVAILABLE = True
except ImportError:
    _THRIFT_AVAILABLE = False


class Bmv2ThriftClient:
    """Thin wrapper around BMv2's Thrift management API for register operations.

    Parameters
    ----------
    host : str
        Hostname or IP of the running simple_switch_grpc instance.
    port : int
        Thrift management port (simple_switch_grpc default: 9090).
    """

    CTX_ID = 0  # simple_switch default pipeline context

    def __init__(self, host: str, port: int):
        if not _THRIFT_AVAILABLE:
            raise RuntimeError(
                "bm_runtime / thrift packages not found; "
                "install them to use Bmv2ThriftClient"
            )
        transport = TSocket.TSocket(host, port)
        self._transport = TTransport.TBufferedTransport(transport)
        protocol = TBinaryProtocol.TBinaryProtocol(self._transport)
        # BMv2 uses a multiplexed Thrift processor; the service name is "standard".
        # Without TMultiplexedProtocol the server drops the frame with
        # "Non-multiplexed client request dropped."
        multi_protocol = TMultiplexedProtocol.TMultiplexedProtocol(
            protocol, "standard")
        self._client = Standard.Client(multi_protocol)
        self._transport.open()
        LOGGER.debug("Bmv2ThriftClient connected to %s:%d", host, port)

    def clear_register(self, register_name: str) -> None:
        """Reset all cells of a register array to zero."""
        self._client.bm_register_reset(self.CTX_ID, register_name)

    def read_all(self, register_name: str) -> List[int]:
        """Return all cell values of a register array as a list of ints."""
        return self._client.bm_register_read_all(self.CTX_ID, register_name)

    def is_alive(self) -> bool:
        """Return True if the Thrift server is reachable and responding.

        Uses bm_get_config_md5() as a zero-argument probe RPC.
        """
        try:
            self._client.bm_get_config_md5()
            return True
        except Exception:
            return False

    def close(self) -> None:
        """Close the Thrift transport."""
        try:
            self._transport.close()
        except Exception:
            pass
