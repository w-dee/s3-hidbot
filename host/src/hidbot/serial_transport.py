"""Board-safe pyserial byte transport for the U3.3 host client."""

from __future__ import annotations

import math
import time
from collections.abc import Callable
from typing import Any, Protocol

import serial

from .errors import TransportError


class SerialLike(Protocol):
    """The small pyserial surface used by this transport."""

    dtr: bool
    rts: bool
    port: str | None

    def open(self) -> None: ...

    def close(self) -> None: ...

    def read(self, size: int) -> bytes: ...

    def write(self, data: bytes) -> int: ...


SerialFactory = Callable[..., SerialLike]


class PySerialTransport:
    """Bounded byte transport with the measured FNK0085 line-state policy.

    The transport deliberately does not expose DTR/RTS setters.  Both lines
    are set to the measured safe idle state before the tty is opened and are
    restored to that state immediately before close.
    """

    def __init__(
        self,
        port: str,
        baudrate: int,
        *,
        read_timeout: float = 0.05,
        write_timeout: float = 1.0,
        serial_factory: SerialFactory = serial.Serial,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        if not isinstance(port, str) or not port:
            raise ValueError("serial port must be a non-empty string")
        if type(baudrate) is not int or baudrate <= 0:
            raise ValueError("baudrate must be a positive integer")
        try:
            read_timeout_value = float(read_timeout)
        except (TypeError, ValueError) as exc:
            raise ValueError("read_timeout must be positive and finite") from exc
        if not math.isfinite(read_timeout_value) or read_timeout_value <= 0:
            raise ValueError("read_timeout must be positive and finite")
        try:
            write_timeout_value = float(write_timeout)
        except (TypeError, ValueError) as exc:
            raise ValueError("write_timeout must be positive and finite") from exc
        if not math.isfinite(write_timeout_value) or write_timeout_value <= 0:
            raise ValueError("write_timeout must be positive and finite")
        self._port = port
        self._baudrate = baudrate
        self._read_timeout = read_timeout_value
        self._write_timeout = write_timeout_value
        self._serial_factory = serial_factory
        self._clock = clock
        self._serial: SerialLike | None = None

    @property
    def is_open(self) -> bool:
        return self._serial is not None

    def open(self) -> None:
        """Open the tty once, applying the safe line state before open."""

        if self._serial is not None:
            raise TransportError("serial transport is already open")

        serial_obj: SerialLike | None = None
        try:
            serial_obj = self._serial_factory(
                port=None,
                baudrate=self._baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self._read_timeout,
                write_timeout=self._write_timeout,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False,
                exclusive=True,
            )
            serial_obj.dtr = True
            serial_obj.rts = True
            serial_obj.port = self._port
            serial_obj.open()
        except Exception as exc:
            if serial_obj is not None:
                try:
                    serial_obj.close()
                except Exception:
                    pass
            raise TransportError("could not open serial transport") from exc
        self._serial = serial_obj

    def _require_open(self) -> SerialLike:
        if self._serial is None:
            raise TransportError("serial transport is not open")
        return self._serial

    def read(self, max_bytes: int, timeout: float) -> bytes:
        """Read at most ``max_bytes`` and never wait past the requested bound."""

        if type(max_bytes) is not int or max_bytes < 0:
            raise ValueError("max_bytes must be a non-negative integer")
        try:
            timeout_value = float(timeout)
        except (TypeError, ValueError) as exc:
            raise ValueError("timeout must be non-negative and finite") from exc
        if not math.isfinite(timeout_value) or timeout_value < 0:
            raise ValueError("timeout must be non-negative and finite")
        serial_obj = self._require_open()
        if max_bytes == 0 or timeout_value == 0:
            return b""

        requested_timeout = min(self._read_timeout, timeout_value)
        old_timeout: Any = getattr(serial_obj, "timeout", None)
        changed_timeout = hasattr(serial_obj, "timeout") and old_timeout != requested_timeout
        try:
            if changed_timeout:
                serial_obj.timeout = requested_timeout  # type: ignore[attr-defined]
            data = serial_obj.read(max_bytes)
        except (serial.SerialException, OSError) as exc:
            raise TransportError("serial read failed") from exc
        except Exception as exc:
            raise TransportError("serial read failed") from exc
        finally:
            if changed_timeout:
                try:
                    serial_obj.timeout = old_timeout  # type: ignore[attr-defined]
                except Exception as exc:
                    raise TransportError("serial read timeout restore failed") from exc

        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TransportError("serial read returned a non-byte value")
        return bytes(data[:max_bytes])

    def write(self, data: bytes | bytearray | memoryview) -> int:
        """Complete a bounded write, handling pyserial partial-write results."""

        serial_obj = self._require_open()
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TypeError("serial write data must be bytes-like")
        payload = bytes(data)
        if not payload:
            return 0

        view = memoryview(payload)
        offset = 0
        deadline = self._clock() + self._write_timeout
        while offset < len(view):
            if self._clock() >= deadline:
                raise TransportError("serial write timed out")
            try:
                written = serial_obj.write(bytes(view[offset:]))
            except (serial.SerialException, OSError) as exc:
                raise TransportError("serial write failed") from exc
            except Exception as exc:
                raise TransportError("serial write failed") from exc
            if type(written) is not int or written <= 0:
                raise TransportError("serial write made no progress")
            offset += min(written, len(view) - offset)
        return offset

    def close(self) -> None:
        """Restore safe idle and close; repeated/failed-open close is safe."""

        serial_obj = self._serial
        if serial_obj is None:
            return
        error: Exception | None = None
        try:
            for line in ("dtr", "rts"):
                try:
                    setattr(serial_obj, line, True)
                except Exception as exc:
                    if error is None:
                        error = exc
        finally:
            try:
                serial_obj.close()
            except Exception as exc:
                if error is None:
                    error = exc
            self._serial = None
        if error is not None:
            raise TransportError("serial close failed") from error

    def __enter__(self) -> "PySerialTransport":
        self.open()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
