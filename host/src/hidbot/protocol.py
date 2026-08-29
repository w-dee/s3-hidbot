"""Strict, bounded v1 JSON request/response model without serial dependencies."""

from __future__ import annotations

import json
import math
import re
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any, Literal, cast

from .errors import ProtocolError
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
TOKEN_PATTERN = re.compile(r"[0-9a-f]{32}\Z")
REQUIRED_CAPABILITIES = frozenset(
    {
        "protocol.hello-v1",
        "system.ping-v1",
        "system.info-v1",
        "usb.status-v1",
        "hid.lease-v1",
        "hid.release-all-v1",
        "hid.keyboard-report-v1",
        "hid.mouse-report-v1",
    }
)


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
    if (
        not isinstance(result["project"], str)
        or result["project"] != "s3-hidbot"
        or type(result["protocol_version"]) is not int
        or result["protocol_version"] != PROTOCOL_VERSION
    ):
        raise ProtocolError("hello response identifies an incompatible project")
    capabilities = result["capabilities"]
    if (
        not isinstance(capabilities, list)
        or not all(isinstance(item, str) and item for item in capabilities)
        or len(capabilities) > MAX_ARRAY_MEMBERS
        or len(set(capabilities)) != len(capabilities)
        or any(len(item.encode("utf-8")) > MAX_STRING_BYTES for item in capabilities)
        or not REQUIRED_CAPABILITIES.issubset(capabilities)
    ):
        raise ProtocolError("hello response capabilities are incompatible")
    if type(result["lease_ms"]) is not int or result["lease_ms"] != LEASE_MS:
        raise ProtocolError("hello response lease is incompatible")
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
