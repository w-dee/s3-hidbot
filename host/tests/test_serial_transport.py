from __future__ import annotations

import unittest

import serial

from hidbot.errors import TransportError
from hidbot.serial_transport import PySerialTransport


class RecordingSerial:
    def __init__(self, events: list[tuple[str, object]], **kwargs: object) -> None:
        self.events = events
        self.events.append(("construct", kwargs))
        self.timeout = kwargs.get("timeout", 0.05)
        self.read_values: list[bytes] = []
        self.write_values: list[bytes] = []
        self.write_results: list[int] = []
        self.open_error: Exception | None = None
        self._dtr = None
        self._rts = None
        self._port = None

    @property
    def dtr(self) -> bool | None:
        return self._dtr

    @dtr.setter
    def dtr(self, value: bool) -> None:
        self._dtr = value
        if hasattr(self, "events"):
            self.events.append(("dtr", value))

    @property
    def rts(self) -> bool | None:
        return self._rts

    @rts.setter
    def rts(self, value: bool) -> None:
        self._rts = value
        if hasattr(self, "events"):
            self.events.append(("rts", value))

    @property
    def port(self) -> str | None:
        return self._port

    @port.setter
    def port(self, value: str | None) -> None:
        self._port = value
        if hasattr(self, "events"):
            self.events.append(("port", value))

    def open(self) -> None:
        self.events.append(("open", None))
        if self.open_error is not None:
            raise self.open_error

    def close(self) -> None:
        self.events.append(("close", None))

    def read(self, size: int) -> bytes:
        self.events.append(("read", size))
        return self.read_values.pop(0) if self.read_values else b""

    def write(self, data: bytes) -> int:
        self.events.append(("write", data))
        self.write_values.append(data)
        return self.write_results.pop(0) if self.write_results else len(data)


class SerialTransportTests(unittest.TestCase):
    def make_transport(self, events: list[tuple[str, object]]) -> tuple[PySerialTransport, RecordingSerial]:
        holder: list[RecordingSerial] = []

        def factory(**kwargs: object) -> RecordingSerial:
            serial_obj = RecordingSerial(events, **kwargs)
            holder.append(serial_obj)
            return serial_obj

        transport = PySerialTransport("dummy-port", 115200, serial_factory=factory)
        transport.open()
        return transport, holder[0]

    def test_open_order_and_serial_configuration(self) -> None:
        events: list[tuple[str, object]] = []
        transport, _ = self.make_transport(events)
        self.assertEqual(
            [event[0] for event in events],
            ["construct", "dtr", "rts", "port", "open"],
        )
        kwargs = events[0][1]
        assert isinstance(kwargs, dict)
        self.assertIsNone(kwargs["port"])
        self.assertEqual(kwargs["baudrate"], 115200)
        self.assertEqual(kwargs["bytesize"], serial.EIGHTBITS)
        self.assertEqual(kwargs["parity"], serial.PARITY_NONE)
        self.assertEqual(kwargs["stopbits"], serial.STOPBITS_ONE)
        self.assertFalse(kwargs["xonxoff"])
        self.assertFalse(kwargs["rtscts"])
        self.assertFalse(kwargs["dsrdtr"])
        self.assertTrue(kwargs["exclusive"])
        self.assertEqual([event[1] for event in events if event[0] in ("dtr", "rts")], [True, True])
        self.assertTrue(transport.is_open)

    def test_close_restores_safe_lines_and_is_idempotent(self) -> None:
        events: list[tuple[str, object]] = []
        transport, _ = self.make_transport(events)
        events.clear()
        transport.close()
        transport.close()
        self.assertEqual(events, [("dtr", True), ("rts", True), ("close", None)])
        self.assertFalse(transport.is_open)

    def test_read_is_bounded_and_clamped(self) -> None:
        events: list[tuple[str, object]] = []
        transport, serial_obj = self.make_transport(events)
        serial_obj.read_values.append(b"abcdef")
        self.assertEqual(transport.read(3, 0.01), b"abc")
        self.assertEqual(serial_obj.timeout, 0.05)
        self.assertEqual([event for event in events if event[0] == "read"], [("read", 3)])

    def test_write_completes_partial_writes(self) -> None:
        events: list[tuple[str, object]] = []
        transport, serial_obj = self.make_transport(events)
        serial_obj.write_results[:] = [2, 2]
        self.assertEqual(transport.write(b"abcd"), 4)
        self.assertEqual(serial_obj.write_values, [b"abcd", b"cd"])
        self.assertEqual(transport.write(b""), 0)

    def test_errors_map_and_failed_open_is_cleaned_up(self) -> None:
        events: list[tuple[str, object]] = []
        serial_obj = RecordingSerial(events)
        serial_obj.open_error = OSError("unavailable")
        transport = PySerialTransport("dummy-port", 115200, serial_factory=lambda **_: serial_obj)
        with self.assertRaises(TransportError):
            transport.open()
        self.assertEqual(events[-1][0], "close")
        self.assertFalse(transport.is_open)

        events.clear()
        transport, serial_obj = self.make_transport(events)
        serial_obj.read_values.append("not bytes")  # type: ignore[arg-type]
        with self.assertRaises(TransportError):
            transport.read(10, 0.05)


if __name__ == "__main__":
    unittest.main()
