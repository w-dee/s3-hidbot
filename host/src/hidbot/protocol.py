"""Strict, bounded v1 JSON request/response model without serial dependencies."""

from __future__ import annotations

import json
import math
import re
from collections.abc import Collection, Sequence
from dataclasses import dataclass
from enum import Enum
from typing import Any, Literal, cast

from .errors import CompatibilityError, ProtocolError
from .framing import FRAME_PREFIX, MAX_MACHINE_FRAME_BYTES


PROTOCOL_VERSION = 1
LEASE_MS = 5000
MAX_REQUEST_FRAME_BYTES = 512
MAX_ID = 2_147_483_647
MAX_TOKEN_LENGTH = 32
MAX_JSON_DEPTH = 8
MAX_OBJECT_MEMBERS = 16
MAX_ARRAY_MEMBERS = 16
MAX_STRING_BYTES = 256
MAX_FIRMWARE_VERSION_BYTES = 31
MAX_SOURCE_REVISION_BYTES = 40
MAX_APP_ELF_SHA256_BYTES = 64
MAX_BUILD_PROFILE_BYTES = 31
MAX_USB_GENERATION = 0xFFFF_FFFF
MAX_UINT32 = 0xFFFF_FFFF
MAX_BLE_KEY_SIZE = 16
BLE_PAIRING_TRANSACTION_CAPABILITY = "ble.pairing-transaction-v1"
BLE_BOND_ADMINISTRATION_CAPABILITY = "ble.bond-administration-v1"
MAX_BONDS = 3
HID_OUTPUT_ROUTE_V1_CAPABILITY = "hid.output-route-v1"
HID_OUTPUT_ROUTE_V2_CAPABILITY = "hid.output-route-v2"
TOKEN_PATTERN = re.compile(r"[0-9a-f]{32}\Z")
SOURCE_REVISION_PATTERN = re.compile(rf"[0-9a-f]{{{MAX_SOURCE_REVISION_BYTES}}}\Z")
APP_ELF_SHA256_PATTERN = re.compile(rf"[0-9a-f]{{{MAX_APP_ELF_SHA256_BYTES}}}\Z")
BUILD_PROFILE_PATTERN = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")

# The baseline is the minimum safe v1 control plane.  HID report commands are
# optional because a peer can provide safe diagnostics and recovery without
# allowing unsafe input injection.
BASELINE_REQUIRED_CAPABILITIES = frozenset(
    {
        "protocol.hello-v1",
        "system.ping-v1",
        "system.info-v1",
        "usb.status-v1",
        "hid.lease-v1",
        "hid.release-all-v1",
    }
)
OPTIONAL_CAPABILITIES = frozenset(
    {
        "usb.exposure-control-v1",
        "hid.keyboard-report-v1",
        "hid.mouse-report-v1",
        "firmware.identity-v1",
        HID_OUTPUT_ROUTE_V1_CAPABILITY,
        HID_OUTPUT_ROUTE_V2_CAPABILITY,
        "ble.exposure-control-v1",
        BLE_PAIRING_TRANSACTION_CAPABILITY,
        BLE_BOND_ADMINISTRATION_CAPABILITY,
    }
)
KNOWN_OPTIONAL_CAPABILITIES = OPTIONAL_CAPABILITIES

# Kept as a compatibility alias for callers that imported the pre-U6 name.
# Validation now uses BASELINE_REQUIRED_CAPABILITIES and optional capabilities
# independently.
REQUIRED_CAPABILITIES = BASELINE_REQUIRED_CAPABILITIES | {
    "hid.keyboard-report-v1",
    "hid.mouse-report-v1",
}


@dataclass(frozen=True)
class RemoteErrorObject:
    code: str
    message: str


@dataclass(frozen=True)
class Response:
    response_id: int | None
    session: str | None
    ok: bool
    result: Any
    error: RemoteErrorObject | None


@dataclass(frozen=True)
class HelloResponse:
    session: str
    boot_id: str
    client_nonce: str
    project: str
    protocol_version: int
    capabilities: tuple[str, ...]
    lease_ms: int


@dataclass(frozen=True)
class FirmwareIdentity:
    """Validated identity-v1 fields reported by a firmware image."""

    version: str
    source_revision: str | None
    app_elf_sha256: str
    build_profile: str


@dataclass(frozen=True)
class SystemInfo:
    """Validated system.info data, including optional firmware identity."""

    project: str
    target: str
    idf_version: str
    protocol_version: int
    firmware: FirmwareIdentity | None


@dataclass(frozen=True)
class CompatibilityResult:
    """Pure compatibility assessment, not a USB or fixture health result."""

    compatible: bool
    missing_baseline_capabilities: tuple[str, ...]
    advertised_optional_capabilities: tuple[str, ...]
    identity_available: bool
    firmware_identity: FirmwareIdentity | None
    target_supported: bool


_SEMVER_IDENTIFIER = r"(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
_SEMVER_PATTERN = re.compile(
    rf"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
    rf"(?:-(?:{_SEMVER_IDENTIFIER})(?:\.{_SEMVER_IDENTIFIER})*)?"
    rf"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?\Z"
)


@dataclass(frozen=True)
class ReleaseAllResult:
    keyboard: Literal["already_up", "submitted"]
    mouse: Literal["already_up", "submitted"]


@dataclass(frozen=True)
class KeyboardReportResult:
    state: Literal["already_set", "submitted"]


@dataclass(frozen=True)
class MouseReportResult:
    state: Literal["already_set", "submitted"]


@dataclass(frozen=True)
class UsbExposureLastError:
    operation: Literal["install", "uninstall", "runtime"]
    code: int


USB_RUNTIME_ERROR_TINYUSB_DIAGNOSTIC = -0x7601
USB_RUNTIME_ERROR_TINYUSB_EVENT_QUEUE_OVERFLOW = -0x7602
USB_RUNTIME_ERROR_CODES = frozenset(
    {
        USB_RUNTIME_ERROR_TINYUSB_DIAGNOSTIC,
        USB_RUNTIME_ERROR_TINYUSB_EVENT_QUEUE_OVERFLOW,
    }
)


@dataclass(frozen=True)
class UsbExposureStatus:
    """Strict usb.exposure.status-v1 lifecycle snapshot."""

    desired: Literal["hidden", "exposed"]
    observed: Literal[
        "driver_not_installed", "disconnected", "attaching", "mounted", "suspended", "detaching"
    ]
    generation: int
    mounted: bool
    suspended: bool
    keyboard_ready: bool
    mouse_ready: bool
    safety_pending: bool
    host_release_uncertain: bool
    recovery_required: bool
    last_error: UsbExposureLastError | None


class BleExposureDesired(str, Enum):
    HIDDEN = "hidden"
    EXPOSED = "exposed"


class BleExposureObserved(str, Enum):
    UNINITIALIZED = "uninitialized"
    ENABLING = "enabling"
    IDLE = "idle"
    ADVERTISING = "advertising"
    CONNECTED = "connected"
    DISABLING = "disabling"
    FAULT = "fault"


@dataclass(frozen=True)
class BleExposureLastError:
    operation: Literal["enable", "disable", "runtime"]
    code: int


@dataclass(frozen=True)
class BleExposureStatus:
    """Strict ble.exposure-control-v1 transport lifecycle snapshot."""

    desired: BleExposureDesired
    observed: BleExposureObserved
    generation: int
    stack_ready: bool
    advertising: bool
    connected: bool
    recovery_required: bool
    last_error: BleExposureLastError | None


class BlePairingState(str, Enum):
    IDLE = "idle"
    SECURING = "securing"
    WAITING_INPUT = "waiting_input"


class BlePairingAction(str, Enum):
    PASSKEY_INPUT = "passkey_input"


class BlePairingLastResult(str, Enum):
    NONE = "none"
    SUCCEEDED = "succeeded"
    SMP_FAILED = "smp_failed"
    TIMEOUT = "timeout"
    PEER_DISCONNECTED = "peer_disconnected"
    STORE_FULL = "store_full"
    STORAGE = "storage"
    QUEUE_OVERFLOW = "queue_overflow"
    REPEAT_PAIRING = "repeat_pairing"
    SECURITY_POLICY = "security_policy"


@dataclass(frozen=True)
class BlePairingStatus:
    """Strict ble.pairing-transaction-v1 transaction snapshot."""

    state: BlePairingState
    generation: int
    connected: bool
    pairing_id: int | None
    action: BlePairingAction | None
    remaining_ms: int | None
    encrypted: bool
    authenticated: bool
    bonded: bool
    secure_connections: bool
    key_size: int
    last_result: BlePairingLastResult


@dataclass(frozen=True)
class BlePairingRespondResult:
    accepted: bool
    pairing_id: int


@dataclass(frozen=True)
class BleBondInfo:
    """Non-secret administrative view of one firmware-side stored bond."""

    bond_id: str
    our_sec: bool
    peer_sec: bool
    verified: bool
    schema_revision: int | None
    schema_current: bool
    connected: bool


@dataclass(frozen=True)
class BleBondList:
    capacity: int
    count: int
    available: int
    healthy: bool
    bonds: tuple[BleBondInfo, ...]


@dataclass(frozen=True)
class BleBondRemoveResult:
    bond_id: str
    removed: bool
    remaining: int


class OutputRoute(str, Enum):
    NONE = "none"
    USB = "usb"


@dataclass(frozen=True)
class HidRouteStatus:
    """Strict hid.output-route-v1 transaction snapshot."""

    desired: OutputRoute
    active: OutputRoute
    generation: int
    transition: Literal["stable", "releasing"]
    ready: bool


class OutputRouteV2(str, Enum):
    NONE = "none"
    USB = "usb"
    BLE = "ble"


@dataclass(frozen=True)
class HidRouteV2Status:
    """Strict hid.output-route-v2 transaction snapshot."""

    desired: OutputRouteV2
    active: OutputRouteV2
    generation: int
    transition: Literal["stable", "releasing"]
    ready: bool


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON constant {value}")


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        if key in output:
            raise ValueError(f"duplicate JSON key {key!r}")
        output[key] = value
    return output


def _validate_tree(value: Any, depth: int = 0) -> None:
    if depth > MAX_JSON_DEPTH:
        raise ProtocolError("response JSON nesting exceeds the host bound")
    if isinstance(value, str):
        if len(value.encode("utf-8")) > MAX_STRING_BYTES:
            raise ProtocolError("response JSON string exceeds the host bound")
        return
    if value is None or isinstance(value, bool):
        return
    if isinstance(value, int):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ProtocolError("response JSON number is not finite")
        return
    if isinstance(value, list):
        if len(value) > MAX_ARRAY_MEMBERS:
            raise ProtocolError("response JSON array exceeds the host bound")
        for child in value:
            _validate_tree(child, depth + 1)
        return
    if isinstance(value, dict):
        if len(value) > MAX_OBJECT_MEMBERS:
            raise ProtocolError("response JSON object exceeds the host bound")
        for key, child in value.items():
            if not isinstance(key, str) or len(key.encode("utf-8")) > MAX_STRING_BYTES:
                raise ProtocolError("response JSON key exceeds the host bound")
            _validate_tree(child, depth + 1)
        return
    raise ProtocolError("response JSON contains an unsupported value")


def _token(value: Any, field: str, *, allow_null: bool = True) -> str | None:
    if value is None and allow_null:
        return None
    if not isinstance(value, str) or TOKEN_PATTERN.fullmatch(value) is None:
        raise ProtocolError(f"{field} is not a lowercase 32-hex token")
    return value


def _response_id(value: Any) -> int | None:
    if value is None:
        return None
    if type(value) is not int or not 0 <= value <= MAX_ID:
        raise ProtocolError("response id is not a valid int32")
    return value


def parse_response(payload: bytes) -> Response:
    """Decode one complete machine payload with duplicate-key rejection."""

    if len(FRAME_PREFIX) + len(payload) + 1 > MAX_MACHINE_FRAME_BYTES:
        raise ProtocolError("machine response exceeds the host bound")
    try:
        text = payload.decode("utf-8")
        value = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as exc:
        raise ProtocolError("machine response is not valid JSON") from exc
    _validate_tree(value)
    if not isinstance(value, dict):
        raise ProtocolError("machine response must be an object")

    allowed = {"type", "v", "id", "session", "ok", "result", "error"}
    required = {"type", "v", "id", "session", "ok"}
    if set(value) - allowed or not required.issubset(value) or value.get("type") != "response":
        raise ProtocolError("machine response envelope is invalid")
    if type(value.get("v")) is not int or value["v"] != PROTOCOL_VERSION:
        raise ProtocolError("machine response protocol version is invalid")
    response_id = _response_id(value.get("id"))
    session = _token(value.get("session"), "session")
    if type(value.get("ok")) is not bool:
        raise ProtocolError("machine response ok must be boolean")

    has_result = "result" in value
    has_error = "error" in value
    if has_result == has_error:
        raise ProtocolError("machine response must contain exactly one result or error")
    if has_result:
        if not value["ok"]:
            raise ProtocolError("result response must have ok=true")
        return Response(response_id, session, value["ok"], value["result"], None)

    if value["ok"]:
        raise ProtocolError("error response must have ok=false")
    error = value["error"]
    if not isinstance(error, dict) or set(error) != {"code", "message"}:
        raise ProtocolError("machine response error object is invalid")
    code = error["code"]
    message = error["message"]
    if (
        not isinstance(code, str)
        or not code
        or len(code.encode("utf-8")) > MAX_STRING_BYTES
        or not isinstance(message, str)
        or len(message.encode("utf-8")) > MAX_STRING_BYTES
    ):
        raise ProtocolError("machine response error fields are invalid")
    return Response(response_id, session, value["ok"], None, RemoteErrorObject(code, message))


def _serialize_request(value: dict[str, Any]) -> bytes:
    try:
        payload = json.dumps(
            value,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError) as exc:
        raise ProtocolError("request cannot be serialized as strict JSON") from exc
    frame = FRAME_PREFIX + payload + b"\n"
    if len(frame) > MAX_REQUEST_FRAME_BYTES:
        raise ProtocolError("request frame exceeds the firmware bound")
    return frame


def build_hello_frame(request_id: int, client_nonce: str) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("hello request id is invalid")
    if not isinstance(client_nonce, str) or TOKEN_PATTERN.fullmatch(client_nonce) is None:
        raise ProtocolError("client nonce is invalid")
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "cmd": "protocol.hello",
            "params": {"client_nonce": client_nonce},
        }
    )


def build_command_frame(request_id: int, session: str, command: str) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    if not isinstance(command, str) or not command:
        raise ProtocolError("command is invalid")
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": command,
            "params": {},
        }
    )


def build_hid_route_set_frame(
    request_id: int, session: str, route: OutputRoute
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    if not isinstance(route, OutputRoute):
        raise ProtocolError("HID output route is invalid")
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "hid.route.set",
            "params": {"route": route.value},
        }
    )


def build_hid_route_v2_set_frame(
    request_id: int, session: str, route: OutputRouteV2
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    if not isinstance(route, OutputRouteV2):
        raise ProtocolError("HID output route v2 is invalid")
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "hid.route.v2.set",
            "params": {"route": route.value},
        }
    )


def validate_ble_pairing_respond_inputs(pairing_id: int, passkey: str) -> None:
    if type(pairing_id) is not int or not 1 <= pairing_id <= MAX_UINT32:
        raise ProtocolError("BLE pairing ID is invalid")
    if (
        type(passkey) is not str
        or len(passkey) != 6
        or any(character < "0" or character > "9" for character in passkey)
    ):
        raise ProtocolError("BLE pairing passkey must be exactly six ASCII digits")


def build_ble_pairing_respond_frame(
    request_id: int, session: str, pairing_id: int, passkey: str
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    validate_ble_pairing_respond_inputs(pairing_id, passkey)
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "ble.pairing.respond",
            "params": {"pairing_id": pairing_id, "passkey": passkey},
        }
    )


def validate_bond_id(bond_id: str) -> None:
    if type(bond_id) is not str or TOKEN_PATTERN.fullmatch(bond_id) is None:
        raise ProtocolError("BLE bond ID must be exactly 32 lowercase hexadecimal characters")


def build_ble_bond_remove_frame(
    request_id: int, session: str, bond_id: str
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    validate_bond_id(bond_id)
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "ble.bond.remove",
            "params": {"bond_id": bond_id},
        }
    )


def validate_keyboard_report_inputs(modifiers: int, keys: Sequence[int]) -> list[int]:
    if type(modifiers) is not int or not 0 <= modifiers <= 255:
        raise ProtocolError("keyboard report modifiers are invalid")
    if isinstance(keys, (str, bytes, bytearray)) or not isinstance(keys, Sequence):
        raise ProtocolError("keyboard report keys are invalid")
    try:
        values = list(keys)
    except TypeError as exc:
        raise ProtocolError("keyboard report keys are invalid") from exc
    if len(values) > 6:
        raise ProtocolError("keyboard report keys exceed six entries")
    previous = -1
    for value in values:
        if type(value) is not int or not 0 <= value <= 255:
            raise ProtocolError("keyboard report key usage is invalid")
        if value <= previous:
            raise ProtocolError("keyboard report keys must be strictly ascending")
        if not (0x04 <= value <= 0xA4 or 0xB0 <= value <= 0xDD):
            raise ProtocolError("keyboard report key usage is not allowed")
        previous = value
    return values


def build_keyboard_report_frame(
    request_id: int, session: str, modifiers: int, keys: Sequence[int]
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    values = validate_keyboard_report_inputs(modifiers, keys)
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "hid.keyboard.report",
            "params": {"modifiers": modifiers, "keys": values},
        }
    )


def validate_mouse_report_inputs(buttons: int, x: int, y: int, wheel: int, pan: int) -> None:
    if type(buttons) is not int or not 0 <= buttons <= 31:
        raise ProtocolError("mouse report buttons are invalid")
    for name, value in (("x", x), ("y", y), ("wheel", wheel), ("pan", pan)):
        if type(value) is not int or not -127 <= value <= 127:
            raise ProtocolError(f"mouse report {name} is invalid")


def build_mouse_report_frame(
    request_id: int,
    session: str,
    buttons: int,
    x: int,
    y: int,
    wheel: int,
    pan: int,
) -> bytes:
    if type(request_id) is not int or not 0 <= request_id <= MAX_ID:
        raise ProtocolError("request id is invalid")
    if not isinstance(session, str) or TOKEN_PATTERN.fullmatch(session) is None:
        raise ProtocolError("session is invalid")
    validate_mouse_report_inputs(buttons, x, y, wheel, pan)
    return _serialize_request(
        {
            "v": PROTOCOL_VERSION,
            "id": request_id,
            "session": session,
            "cmd": "hid.mouse.report",
            "params": {"buttons": buttons, "x": x, "y": y, "wheel": wheel, "pan": pan},
        }
    )


def validate_hello_response(
    response: Response,
    *,
    expected_id: int,
    expected_nonce: str,
) -> HelloResponse:
    """Validate all fields needed to establish a current session."""

    if response.response_id != expected_id or not response.ok:
        raise ProtocolError("response is not a successful hello")
    if response.session is None or not isinstance(response.result, dict):
        raise ProtocolError("hello response session/result is invalid")
    result = response.result
    required = {
        "project",
        "protocol_version",
        "client_nonce",
        "boot_id",
        "session",
        "capabilities",
        "lease_ms",
    }
    if set(result) != required:
        raise ProtocolError("hello result fields are invalid")
    result_session = _token(result["session"], "result.session", allow_null=False)
    boot_id = _token(result["boot_id"], "boot_id", allow_null=False)
    nonce = _token(result["client_nonce"], "client_nonce", allow_null=False)
    if result_session != response.session or nonce != expected_nonce:
        raise ProtocolError("hello response correlation is invalid")
    if not isinstance(result["project"], str) or type(result["protocol_version"]) is not int:
        raise ProtocolError("hello response project/version types are invalid")
    if (
        result["project"] != "s3-hidbot"
        or result["protocol_version"] != PROTOCOL_VERSION
    ):
        raise CompatibilityError("hello response identifies an incompatible project")
    capabilities = result["capabilities"]
    if (
        not isinstance(capabilities, list)
        or not all(isinstance(item, str) and item for item in capabilities)
        or len(capabilities) > MAX_ARRAY_MEMBERS
        or len(set(capabilities)) != len(capabilities)
        or any(len(item.encode("utf-8")) > MAX_STRING_BYTES for item in capabilities)
    ):
        raise ProtocolError("hello response capabilities are incompatible")
    if not BASELINE_REQUIRED_CAPABILITIES.issubset(capabilities):
        raise CompatibilityError("hello response is missing baseline capabilities")
    if type(result["lease_ms"]) is not int:
        raise ProtocolError("hello response lease type is invalid")
    if result["lease_ms"] != LEASE_MS:
        raise CompatibilityError("hello response lease is incompatible")
    return HelloResponse(
        session=response.session,
        boot_id=boot_id,
        client_nonce=nonce,
        project=result["project"],
        protocol_version=result["protocol_version"],
        capabilities=tuple(capabilities),
        lease_ms=result["lease_ms"],
    )


def validate_release_all_result(value: Any) -> ReleaseAllResult:
    """Validate the strict per-interface release_all result object."""

    if not isinstance(value, dict) or set(value) != {"keyboard", "mouse"}:
        raise ProtocolError("hid.release_all result fields are invalid")
    keyboard = value["keyboard"]
    mouse = value["mouse"]
    if keyboard not in {"already_up", "submitted"} or mouse not in {"already_up", "submitted"}:
        raise ProtocolError("hid.release_all result state is invalid")
    return ReleaseAllResult(
        keyboard=cast(Literal["already_up", "submitted"], keyboard),
        mouse=cast(Literal["already_up", "submitted"], mouse),
    )


def validate_keyboard_report_result(value: Any) -> KeyboardReportResult:
    if not isinstance(value, dict) or set(value) != {"state"}:
        raise ProtocolError("hid.keyboard.report result fields are invalid")
    state = value["state"]
    if state not in {"already_set", "submitted"}:
        raise ProtocolError("hid.keyboard.report result state is invalid")
    return KeyboardReportResult(state=cast(Literal["already_set", "submitted"], state))


def validate_mouse_report_result(value: Any) -> MouseReportResult:
    if not isinstance(value, dict) or set(value) != {"state"}:
        raise ProtocolError("hid.mouse.report result fields are invalid")
    state = value["state"]
    if state not in {"already_set", "submitted"}:
        raise ProtocolError("hid.mouse.report result state is invalid")
    return MouseReportResult(state=cast(Literal["already_set", "submitted"], state))


def validate_usb_exposure_status(value: Any) -> UsbExposureStatus:
    """Validate the exact v1 lifecycle status shape returned by all USB exposure commands."""

    fields = {
        "desired",
        "observed",
        "generation",
        "mounted",
        "suspended",
        "keyboard_ready",
        "mouse_ready",
        "safety_pending",
        "host_release_uncertain",
        "recovery_required",
        "last_error",
    }
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("usb.exposure.status result fields are invalid")
    desired = value["desired"]
    observed = value["observed"]
    generation = value["generation"]
    if desired not in {"hidden", "exposed"}:
        raise ProtocolError("USB exposure desired state is invalid")
    if observed not in {
        "driver_not_installed",
        "disconnected",
        "attaching",
        "mounted",
        "suspended",
        "detaching",
    }:
        raise ProtocolError("USB exposure observed state is invalid")
    if type(generation) is not int or not 0 <= generation <= MAX_USB_GENERATION:
        raise ProtocolError("USB exposure generation is invalid")
    bool_fields = (
        "mounted",
        "suspended",
        "keyboard_ready",
        "mouse_ready",
        "safety_pending",
        "host_release_uncertain",
        "recovery_required",
    )
    if any(type(value[name]) is not bool for name in bool_fields):
        raise ProtocolError("USB exposure boolean state is invalid")
    raw_last_error = value["last_error"]
    last_error: UsbExposureLastError | None = None
    if raw_last_error is not None:
        if not isinstance(raw_last_error, dict) or set(raw_last_error) != {"operation", "code"}:
            raise ProtocolError("USB exposure last_error fields are invalid")
        operation = raw_last_error["operation"]
        code = raw_last_error["code"]
        if operation not in {"install", "uninstall", "runtime"}:
            raise ProtocolError("USB exposure last_error operation is invalid")
        if type(code) is not int or not -(2**31) <= code <= 2**31 - 1:
            raise ProtocolError("USB exposure last_error code is invalid")
        if operation == "runtime" and code not in USB_RUNTIME_ERROR_CODES:
            raise ProtocolError("USB exposure runtime error code is invalid")
        last_error = UsbExposureLastError(
            operation=cast(Literal["install", "uninstall", "runtime"], operation),
            code=code,
        )
    return UsbExposureStatus(
        desired=cast(Literal["hidden", "exposed"], desired),
        observed=cast(
            Literal[
                "driver_not_installed",
                "disconnected",
                "attaching",
                "mounted",
                "suspended",
                "detaching",
            ],
            observed,
        ),
        generation=generation,
        mounted=value["mounted"],
        suspended=value["suspended"],
        keyboard_ready=value["keyboard_ready"],
        mouse_ready=value["mouse_ready"],
        safety_pending=value["safety_pending"],
        host_release_uncertain=value["host_release_uncertain"],
        recovery_required=value["recovery_required"],
        last_error=last_error,
    )


def validate_ble_exposure_status(value: Any) -> BleExposureStatus:
    fields = {
        "desired",
        "observed",
        "generation",
        "stack_ready",
        "advertising",
        "connected",
        "recovery_required",
        "last_error",
    }
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("ble.exposure.status result fields are invalid")
    try:
        desired = BleExposureDesired(value["desired"])
        observed = BleExposureObserved(value["observed"])
    except (TypeError, ValueError) as exc:
        raise ProtocolError("BLE exposure lifecycle state is invalid") from exc
    generation = value["generation"]
    if type(generation) is not int or not 0 <= generation <= MAX_USB_GENERATION:
        raise ProtocolError("BLE exposure generation is invalid")
    bool_fields = ("stack_ready", "advertising", "connected", "recovery_required")
    if any(type(value[name]) is not bool for name in bool_fields):
        raise ProtocolError("BLE exposure boolean state is invalid")
    if observed is BleExposureObserved.UNINITIALIZED and (
        value["stack_ready"] or value["advertising"] or value["connected"]
    ):
        raise ProtocolError("BLE uninitialized state is incoherent")
    if value["advertising"] and value["connected"]:
        raise ProtocolError("BLE cannot advertise and be connected simultaneously")
    raw_error = value["last_error"]
    last_error: BleExposureLastError | None = None
    if raw_error is not None:
        if not isinstance(raw_error, dict) or set(raw_error) != {"operation", "code"}:
            raise ProtocolError("BLE exposure last_error fields are invalid")
        operation = raw_error["operation"]
        code = raw_error["code"]
        if operation not in {"enable", "disable", "runtime"}:
            raise ProtocolError("BLE exposure last_error operation is invalid")
        if type(code) is not int or not -(2**31) <= code <= 2**31 - 1:
            raise ProtocolError("BLE exposure last_error code is invalid")
        last_error = BleExposureLastError(
            operation=cast(Literal["enable", "disable", "runtime"], operation),
            code=code,
        )
    return BleExposureStatus(
        desired=desired,
        observed=observed,
        generation=generation,
        stack_ready=value["stack_ready"],
        advertising=value["advertising"],
        connected=value["connected"],
        recovery_required=value["recovery_required"],
        last_error=last_error,
    )


def validate_ble_pairing_status(value: Any) -> BlePairingStatus:
    fields = {
        "state",
        "generation",
        "connected",
        "pairing_id",
        "action",
        "remaining_ms",
        "encrypted",
        "authenticated",
        "bonded",
        "secure_connections",
        "key_size",
        "last_result",
    }
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("ble.pairing.status result fields are invalid")
    try:
        state = BlePairingState(value["state"])
        last_result = BlePairingLastResult(value["last_result"])
    except (TypeError, ValueError) as exc:
        raise ProtocolError("BLE pairing state is invalid") from exc
    generation = value["generation"]
    pairing_id = value["pairing_id"]
    remaining_ms = value["remaining_ms"]
    key_size = value["key_size"]
    if type(generation) is not int or not 0 <= generation <= MAX_UINT32:
        raise ProtocolError("BLE pairing generation is invalid")
    if pairing_id is not None and (
        type(pairing_id) is not int or not 1 <= pairing_id <= MAX_UINT32
    ):
        raise ProtocolError("BLE pairing ID is invalid")
    if remaining_ms is not None and (
        type(remaining_ms) is not int or not 0 <= remaining_ms <= MAX_UINT32
    ):
        raise ProtocolError("BLE pairing remaining time is invalid")
    if type(key_size) is not int or not 0 <= key_size <= MAX_BLE_KEY_SIZE:
        raise ProtocolError("BLE pairing key size is invalid")
    bool_fields = (
        "connected",
        "encrypted",
        "authenticated",
        "bonded",
        "secure_connections",
    )
    if any(type(value[name]) is not bool for name in bool_fields):
        raise ProtocolError("BLE pairing boolean state is invalid")
    raw_action = value["action"]
    if raw_action is None:
        action = None
    else:
        try:
            action = BlePairingAction(raw_action)
        except (TypeError, ValueError) as exc:
            raise ProtocolError("BLE pairing action is invalid") from exc
    if state is BlePairingState.WAITING_INPUT:
        if (
            pairing_id is None
            or action is not BlePairingAction.PASSKEY_INPUT
            or remaining_ms is None
        ):
            raise ProtocolError("BLE pairing waiting-input fields are incoherent")
    elif pairing_id is not None or action is not None or remaining_ms is not None:
        raise ProtocolError("BLE pairing inactive fields must be null")
    return BlePairingStatus(
        state=state,
        generation=generation,
        connected=value["connected"],
        pairing_id=pairing_id,
        action=action,
        remaining_ms=remaining_ms,
        encrypted=value["encrypted"],
        authenticated=value["authenticated"],
        bonded=value["bonded"],
        secure_connections=value["secure_connections"],
        key_size=key_size,
        last_result=last_result,
    )


def validate_ble_pairing_respond_result(value: Any) -> BlePairingRespondResult:
    if not isinstance(value, dict) or set(value) != {"accepted", "pairing_id"}:
        raise ProtocolError("ble.pairing.respond result fields are invalid")
    accepted = value["accepted"]
    pairing_id = value["pairing_id"]
    if accepted is not True:
        raise ProtocolError("BLE pairing response acceptance is invalid")
    if type(pairing_id) is not int or not 1 <= pairing_id <= MAX_UINT32:
        raise ProtocolError("BLE pairing response ID is invalid")
    return BlePairingRespondResult(accepted=True, pairing_id=pairing_id)


def validate_ble_bond_list(value: Any) -> BleBondList:
    fields = {"capacity", "count", "available", "healthy", "bonds"}
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("ble.bond.list result fields are invalid")
    capacity = value["capacity"]
    count = value["count"]
    available = value["available"]
    healthy = value["healthy"]
    raw_bonds = value["bonds"]
    if capacity != MAX_BONDS or type(capacity) is not int:
        raise ProtocolError("BLE bond capacity is invalid")
    if type(count) is not int or not 0 <= count <= MAX_BONDS:
        raise ProtocolError("BLE bond count is invalid")
    if type(available) is not int or available != MAX_BONDS - count:
        raise ProtocolError("BLE bond available capacity is invalid")
    if type(healthy) is not bool:
        raise ProtocolError("BLE bond store health is invalid")
    if not isinstance(raw_bonds, list) or len(raw_bonds) != count:
        raise ProtocolError("BLE bond list length is invalid")
    bonds: list[BleBondInfo] = []
    expected_fields = {
        "bond_id",
        "our_sec",
        "peer_sec",
        "verified",
        "schema_revision",
        "schema_current",
        "connected",
    }
    for raw in raw_bonds:
        if not isinstance(raw, dict) or set(raw) != expected_fields:
            raise ProtocolError("BLE bond entry fields are invalid")
        validate_bond_id(raw["bond_id"])
        bool_fields = (
            "our_sec",
            "peer_sec",
            "verified",
            "schema_current",
            "connected",
        )
        if any(type(raw[name]) is not bool for name in bool_fields):
            raise ProtocolError("BLE bond entry boolean state is invalid")
        revision = raw["schema_revision"]
        if revision is not None and (
            type(revision) is not int or not 0 <= revision <= 255
        ):
            raise ProtocolError("BLE bond schema revision is invalid")
        if raw["schema_current"] and revision is None:
            raise ProtocolError("BLE bond schema state is incoherent")
        if raw["verified"] and not (raw["our_sec"] and raw["peer_sec"]):
            raise ProtocolError("BLE bond verification state is incoherent")
        bonds.append(
            BleBondInfo(
                bond_id=raw["bond_id"],
                our_sec=raw["our_sec"],
                peer_sec=raw["peer_sec"],
                verified=raw["verified"],
                schema_revision=revision,
                schema_current=raw["schema_current"],
                connected=raw["connected"],
            )
        )
    ids = [bond.bond_id for bond in bonds]
    if ids != sorted(ids):
        raise ProtocolError("BLE bond list ordering is invalid")
    expected_healthy = all(bond.verified for bond in bonds) and len(ids) == len(set(ids))
    if healthy != expected_healthy:
        raise ProtocolError("BLE bond store health is incoherent")
    return BleBondList(
        capacity=capacity,
        count=count,
        available=available,
        healthy=healthy,
        bonds=tuple(bonds),
    )


def validate_ble_bond_remove_result(value: Any) -> BleBondRemoveResult:
    if not isinstance(value, dict) or set(value) != {
        "bond_id",
        "removed",
        "remaining",
    }:
        raise ProtocolError("ble.bond.remove result fields are invalid")
    validate_bond_id(value["bond_id"])
    if value["removed"] is not True:
        raise ProtocolError("BLE bond removal result is invalid")
    remaining = value["remaining"]
    if type(remaining) is not int or not 0 <= remaining < MAX_BONDS:
        raise ProtocolError("BLE bond removal remaining count is invalid")
    return BleBondRemoveResult(
        bond_id=value["bond_id"], removed=True, remaining=remaining
    )


def validate_hid_route_status(value: Any) -> HidRouteStatus:
    fields = {"desired", "active", "generation", "transition", "ready"}
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("hid.route status result fields are invalid")
    try:
        desired = OutputRoute(value["desired"])
        active = OutputRoute(value["active"])
    except (TypeError, ValueError) as exc:
        raise ProtocolError("HID output route value is invalid") from exc
    generation = value["generation"]
    transition = value["transition"]
    ready = value["ready"]
    if type(generation) is not int or not 0 <= generation <= MAX_USB_GENERATION:
        raise ProtocolError("HID route generation is invalid")
    if transition not in {"stable", "releasing"}:
        raise ProtocolError("HID route transition is invalid")
    if type(ready) is not bool:
        raise ProtocolError("HID route readiness is invalid")
    if transition == "releasing" and not (
        desired is OutputRoute.NONE and active is OutputRoute.USB and not ready
    ):
        raise ProtocolError("HID route releasing state is invalid")
    if transition == "stable" and desired is not active:
        raise ProtocolError("HID route stable state is invalid")
    if active is OutputRoute.NONE and ready:
        raise ProtocolError("HID route none cannot be ready")
    return HidRouteStatus(
        desired=desired,
        active=active,
        generation=generation,
        transition=cast(Literal["stable", "releasing"], transition),
        ready=ready,
    )


def validate_hid_route_v2_status(value: Any) -> HidRouteV2Status:
    fields = {"desired", "active", "generation", "transition", "ready"}
    if not isinstance(value, dict) or set(value) != fields:
        raise ProtocolError("hid.route.v2 status result fields are invalid")
    try:
        desired = OutputRouteV2(value["desired"])
        active = OutputRouteV2(value["active"])
    except (TypeError, ValueError) as exc:
        raise ProtocolError("HID output route v2 value is invalid") from exc
    generation = value["generation"]
    transition = value["transition"]
    ready = value["ready"]
    if type(generation) is not int or not 0 <= generation <= MAX_USB_GENERATION:
        raise ProtocolError("HID route generation is invalid")
    if transition not in {"stable", "releasing"}:
        raise ProtocolError("HID route transition is invalid")
    if type(ready) is not bool:
        raise ProtocolError("HID route readiness is invalid")
    if transition == "releasing" and not (
        desired is OutputRouteV2.NONE
        and active in {OutputRouteV2.USB, OutputRouteV2.BLE}
        and not ready
    ):
        raise ProtocolError("HID route v2 releasing state is invalid")
    if transition == "stable" and desired is not active:
        raise ProtocolError("HID route stable state is invalid")
    if active is OutputRouteV2.NONE and ready:
        raise ProtocolError("HID route none cannot be ready")
    return HidRouteV2Status(
        desired=desired,
        active=active,
        generation=generation,
        transition=cast(Literal["stable", "releasing"], transition),
        ready=ready,
    )


def _bounded_ascii(value: Any, maximum: int) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError:
        return False
    return len(encoded) <= maximum


def _parse_firmware_identity(value: Any) -> FirmwareIdentity:
    if not isinstance(value, dict) or set(value) != {
        "version",
        "source_revision",
        "app_elf_sha256",
        "build_profile",
    }:
        raise ProtocolError("firmware identity fields are invalid")

    version = value["version"]
    if (
        not _bounded_ascii(version, MAX_FIRMWARE_VERSION_BYTES)
        or _SEMVER_PATTERN.fullmatch(version) is None
    ):
        raise ProtocolError("firmware identity version is invalid")

    source_revision = value["source_revision"]
    if source_revision is not None and (
        not isinstance(source_revision, str)
        or SOURCE_REVISION_PATTERN.fullmatch(source_revision) is None
    ):
        raise ProtocolError("firmware identity source revision is invalid")

    app_elf_sha256 = value["app_elf_sha256"]
    if (
        not isinstance(app_elf_sha256, str)
        or APP_ELF_SHA256_PATTERN.fullmatch(app_elf_sha256) is None
    ):
        raise ProtocolError("firmware identity ELF SHA256 is invalid")

    build_profile = value["build_profile"]
    if (
        not _bounded_ascii(build_profile, MAX_BUILD_PROFILE_BYTES)
        or BUILD_PROFILE_PATTERN.fullmatch(build_profile) is None
    ):
        raise ProtocolError("firmware identity build profile is invalid")

    return FirmwareIdentity(
        version=version,
        source_revision=source_revision,
        app_elf_sha256=app_elf_sha256,
        build_profile=build_profile,
    )


def validate_system_info(
    value: Any,
    *,
    capabilities: Collection[str],
) -> SystemInfo:
    """Validate legacy or identity-v1 ``system.info`` data without I/O."""

    if isinstance(capabilities, (str, bytes, bytearray)) or not isinstance(
        capabilities, Collection
    ):
        raise ProtocolError("system.info capabilities are invalid")
    try:
        if len(capabilities) > MAX_ARRAY_MEMBERS or len(set(capabilities)) != len(capabilities):
            raise ProtocolError("system.info capabilities are invalid")
        capability_set = set(capabilities)
    except (TypeError, ValueError) as exc:
        raise ProtocolError("system.info capabilities are invalid") from exc
    if not all(
        isinstance(item, str)
        and item
        and len(item.encode("utf-8")) <= MAX_STRING_BYTES
        for item in capability_set
    ):
        raise ProtocolError("system.info capabilities are invalid")
    if not isinstance(value, dict):
        raise ProtocolError("system.info result must be an object")

    base_fields = {"project", "target", "idf_version", "protocol_version"}
    identity_advertised = "firmware.identity-v1" in capability_set
    expected_fields = base_fields | ({"firmware"} if identity_advertised else set())
    if set(value) != expected_fields:
        if identity_advertised:
            raise CompatibilityError(
                "firmware.identity-v1 requires the exact identity system.info shape"
            )
        raise CompatibilityError(
            "legacy system.info cannot contain identity fields without firmware.identity-v1"
        )

    project = value["project"]
    if not isinstance(project, str):
        raise ProtocolError("system.info project has an invalid type")
    if project != "s3-hidbot":
        raise CompatibilityError("system.info identifies an incompatible project")

    target = value["target"]
    if not _bounded_ascii(target, MAX_STRING_BYTES):
        raise ProtocolError("system.info target is invalid")

    idf_version = value["idf_version"]
    if not _bounded_ascii(idf_version, MAX_STRING_BYTES):
        raise ProtocolError("system.info IDF version is invalid")

    protocol_version = value["protocol_version"]
    if type(protocol_version) is not int:
        raise ProtocolError("system.info protocol version has an invalid type")
    if protocol_version != PROTOCOL_VERSION:
        raise CompatibilityError("system.info protocol version is incompatible")

    firmware: FirmwareIdentity | None = None
    if identity_advertised:
        try:
            firmware = _parse_firmware_identity(value["firmware"])
        except ProtocolError as exc:
            raise CompatibilityError(
                "firmware.identity-v1 system.info identity is incompatible"
            ) from exc
    return SystemInfo(
        project=project,
        target=target,
        idf_version=idf_version,
        protocol_version=protocol_version,
        firmware=firmware,
    )


def evaluate_compatibility(
    hello: HelloResponse,
    info: Any,
) -> CompatibilityResult:
    """Assess protocol compatibility, excluding USB and physical health."""

    if not isinstance(hello, HelloResponse):
        raise ProtocolError("compatibility hello value is invalid")
    try:
        capabilities = set(hello.capabilities)
    except (TypeError, ValueError) as exc:
        raise ProtocolError("compatibility capabilities are invalid") from exc
    missing = tuple(sorted(BASELINE_REQUIRED_CAPABILITIES - capabilities))
    optional = tuple(sorted(OPTIONAL_CAPABILITIES & capabilities))
    try:
        system_info = validate_system_info(info, capabilities=hello.capabilities)
    except CompatibilityError:
        return CompatibilityResult(
            compatible=False,
            missing_baseline_capabilities=missing,
            advertised_optional_capabilities=optional,
            identity_available=False,
            firmware_identity=None,
            target_supported=False,
        )
    target_supported = system_info.target == "esp32s3"
    protocol_compatible = (
        hello.project == "s3-hidbot" and hello.protocol_version == PROTOCOL_VERSION
    )
    return CompatibilityResult(
        compatible=not missing and protocol_compatible and target_supported,
        missing_baseline_capabilities=missing,
        advertised_optional_capabilities=optional,
        identity_available=system_info.firmware is not None,
        firmware_identity=system_info.firmware,
        target_supported=target_supported,
    )
