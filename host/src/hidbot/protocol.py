"""Strict, bounded v1 JSON request/response model without serial dependencies."""

from __future__ import annotations

import json
import math
import re
from collections.abc import Collection, Sequence
from dataclasses import dataclass
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
        "hid.keyboard-report-v1",
        "hid.mouse-report-v1",
        "firmware.identity-v1",
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
