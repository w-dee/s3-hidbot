"""Pure Python host-side control-plane core for s3-hidbot."""

from typing import TYPE_CHECKING, Any

from .client import Client, HelloResult
from .protocol import (
    CompatibilityResult,
    BleExposureDesired,
    BleExposureLastError,
    BleExposureObserved,
    BleExposureStatus,
    BleBondInfo,
    BleBondList,
    BleBondRemoveResult,
    BlePairingAction,
    BlePairingLastResult,
    BlePairingRespondResult,
    BlePairingState,
    BlePairingStatus,
    FirmwareIdentity,
    KeyboardReportResult,
    MouseReportResult,
    ReleaseAllResult,
    UsbExposureLastError,
    UsbExposureStatus,
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

if TYPE_CHECKING:
    from .serial_transport import PySerialTransport


def __getattr__(name: str) -> Any:
    """Load the optional hardware transport only when the public API is used."""

    if name == "PySerialTransport":
        from .serial_transport import PySerialTransport

        globals()[name] = PySerialTransport
        return PySerialTransport
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

__all__ = [
    "Client",
    "HelloResult",
    "ReleaseAllResult",
    "KeyboardReportResult",
    "MouseReportResult",
    "UsbExposureLastError",
    "UsbExposureStatus",
    "BleExposureDesired",
    "BleExposureObserved",
    "BleExposureLastError",
    "BleExposureStatus",
    "BleBondInfo",
    "BleBondList",
    "BleBondRemoveResult",
    "BlePairingAction",
    "BlePairingLastResult",
    "BlePairingRespondResult",
    "BlePairingState",
    "BlePairingStatus",
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
