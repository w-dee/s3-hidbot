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
    FlashExecutionError,
    HidbotError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .flashing import FlashExecutionResult
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
    "FlashExecutionError",
    "FlashExecutionResult",
    "HidbotError",
    "ProtocolError",
    "RemoteError",
    "RequestTimeoutError",
    "SessionLostError",
    "TransportError",
    "PySerialTransport",
]
