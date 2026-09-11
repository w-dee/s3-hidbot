"""Immutable low-level HID sequence construction for the MCU executor."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from .errors import ProtocolError


MAX_CODE_BYTES = 320
MAX_TOKENS = 64
MAX_DEFAULT_DELAY_MS = 1000
MAX_WAIT_MS = 2000
MAX_SCHEDULED_DURATION_MS = 2500


class MouseButton(Enum):
    LEFT = "L"
    RIGHT = "R"
    MIDDLE = "M"
    BACKWARD = "B"
    FORWARD = "F"


@dataclass(frozen=True)
class _Operation:
    opcode: str
    value: int | MouseButton

    def encode(self) -> str:
        value = self.value.value if isinstance(self.value, MouseButton) else str(self.value)
        return f"{self.opcode}{value}"


@dataclass(frozen=True)
class Sequence:
    _operations: tuple[_Operation, ...]

    def __post_init__(self) -> None:
        if not isinstance(self._operations, tuple) or not self._operations:
            raise ProtocolError("HID sequence must contain at least one token")
        if len(self._operations) > MAX_TOKENS:
            raise ProtocolError("HID sequence exceeds 64 tokens")
        default_delay = 0
        scheduled = 0
        for index, operation in enumerate(self._operations):
            if not isinstance(operation, _Operation):
                raise ProtocolError("HID sequence operation is invalid")
            if operation.opcode == "d":
                _validate_int(operation.value, 0, MAX_DEFAULT_DELAY_MS, "default delay")
                default_delay = operation.value
            elif operation.opcode == "w":
                _validate_int(operation.value, 0, MAX_WAIT_MS, "explicit wait")
                scheduled += operation.value
            elif operation.opcode in {"kp", "kr"}:
                _validate_usage(operation.value)
                if index + 1 < len(self._operations):
                    scheduled += default_delay
            elif operation.opcode in {"mp", "mr"}:
                _validate_button(operation.value)
                if index + 1 < len(self._operations):
                    scheduled += default_delay
            else:
                raise ProtocolError("HID sequence operation is invalid")
            if scheduled > MAX_SCHEDULED_DURATION_MS:
                raise ProtocolError("HID sequence exceeds scheduled-duration limit")
        if len(self.encode().encode("ascii")) > MAX_CODE_BYTES:
            raise ProtocolError("HID sequence exceeds 320 encoded bytes")

    def encode(self) -> str:
        return ";".join(operation.encode() for operation in self._operations)


class SequenceBuilder:
    def __init__(self) -> None:
        self._operations: list[_Operation] = []

    def _append(self, operation: _Operation) -> SequenceBuilder:
        if len(self._operations) >= MAX_TOKENS:
            raise ProtocolError("HID sequence exceeds 64 tokens")
        self._operations.append(operation)
        return self

    def set_default_delay_ms(self, milliseconds: int) -> SequenceBuilder:
        _validate_int(milliseconds, 0, MAX_DEFAULT_DELAY_MS, "default delay")
        return self._append(_Operation("d", milliseconds))

    def wait_ms(self, milliseconds: int) -> SequenceBuilder:
        _validate_int(milliseconds, 0, MAX_WAIT_MS, "explicit wait")
        return self._append(_Operation("w", milliseconds))

    def key_press(self, usage: int) -> SequenceBuilder:
        _validate_usage(usage)
        return self._append(_Operation("kp", usage))

    def key_release(self, usage: int) -> SequenceBuilder:
        _validate_usage(usage)
        return self._append(_Operation("kr", usage))

    def mouse_press(self, button: MouseButton) -> SequenceBuilder:
        _validate_button(button)
        return self._append(_Operation("mp", button))

    def mouse_release(self, button: MouseButton) -> SequenceBuilder:
        _validate_button(button)
        return self._append(_Operation("mr", button))

    def build(self) -> Sequence:
        return Sequence(tuple(self._operations))


def _validate_int(value: int, minimum: int, maximum: int, field: str) -> None:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError(f"HID sequence {field} is invalid")


def _validate_usage(usage: object) -> None:
    if type(usage) is not int or not (
        4 <= usage <= 164 or 176 <= usage <= 221 or 224 <= usage <= 231
    ):
        raise ProtocolError("HID sequence keyboard usage is invalid")


def _validate_button(button: object) -> None:
    if not isinstance(button, MouseButton):
        raise ProtocolError("HID sequence mouse button is invalid")
