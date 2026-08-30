"""Pure Python host-side control-plane core for s3-hidbot."""

from .client import Client, HelloResult
from .protocol import (
    CompatibilityResult,
    FirmwareIdentity,
    KeyboardReportResult,
    MouseReportResult,
    ReleaseAllResult,
    SystemInfo,
    evaluate_compatibility,
    validate_system_info,
)
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
    "MouseReportResult",
    "FirmwareIdentity",
    "SystemInfo",
    "CompatibilityResult",
    "evaluate_compatibility",
    "validate_system_info",
    "CompatibilityError",
    "HidbotError",
    "ProtocolError",
    "RemoteError",
    "RequestTimeoutError",
    "SessionLostError",
    "TransportError",
    "PySerialTransport",
]
