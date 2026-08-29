"""Pure Python host-side control-plane core for s3-hidbot."""

from .client import Client, HelloResult
from .protocol import KeyboardReportResult, ReleaseAllResult
from .errors import (
    CompatibilityError,
    HidbotError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .serial_transport import PySerialTransport

__all__ = [
    "Client",
    "HelloResult",
    "ReleaseAllResult",
    "KeyboardReportResult",
    "CompatibilityError",
    "HidbotError",
    "ProtocolError",
    "RemoteError",
    "RequestTimeoutError",
    "SessionLostError",
    "TransportError",
    "PySerialTransport",
]
