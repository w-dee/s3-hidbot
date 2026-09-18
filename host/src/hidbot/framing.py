"""Bounded byte-oriented framing for the mixed UART log/machine stream."""

from __future__ import annotations

from collections.abc import Callable, Iterable
from dataclasses import dataclass


FRAME_PREFIX = b"@HIDBOT "
TRANSPORT_SYNC = b"\x00\x00\x00\x00"
MAX_MACHINE_FRAME_BYTES = 1024


@dataclass(frozen=True)
class MachineFrame:
    """A complete line whose first bytes are the exact machine prefix."""

    payload: bytes


@dataclass(frozen=True)
class MachineFrameIssue:
    """A prefixed machine candidate that was too large to retain."""

    reason: str


FrameEvent = MachineFrame | MachineFrameIssue
LogSink = Callable[[bytes], None]


class Framer:
    """Incrementally split bounded machine frames from ordinary log lines.

    The parser retains at most ``MAX_MACHINE_FRAME_BYTES - 1`` bytes for one
    line.  A line is a machine candidate only when its first bytes exactly
    match ``FRAME_PREFIX``; prefix-like text later in a normal log is ignored.
    """

    def __init__(self, *, log_sink: LogSink | None = None) -> None:
        self._log_sink = log_sink
        self._line = bytearray()
        self._overlong = False
        self._machine_candidate = False

    def reset(self) -> None:
        """Discard a partial line without draining an unbounded stream."""

        self._line.clear()
        self._overlong = False
        self._machine_candidate = False

    def _start_byte(self, byte: int) -> None:
        self._line.append(byte)
        prefix_length = min(len(self._line), len(FRAME_PREFIX))
        self._machine_candidate = bytes(self._line[:prefix_length]) == FRAME_PREFIX[:prefix_length]

    def _finish_line(self) -> FrameEvent | None:
        if self._overlong:
            event: FrameEvent | None = (
                MachineFrameIssue("oversize") if self._machine_candidate else None
            )
        else:
            line = bytes(self._line)
            if line.endswith(b"\r"):
                line = line[:-1]
            if line.startswith(FRAME_PREFIX):
                event = MachineFrame(line[len(FRAME_PREFIX) :])
            else:
                event = None
                if self._log_sink is not None:
                    self._log_sink(line)
        self.reset()
        return event

    def feed(self, data: bytes | bytearray | memoryview) -> tuple[FrameEvent, ...]:
        """Consume arbitrary bytes and return complete machine events only."""

        events: list[FrameEvent] = []
        for byte in bytes(data):
            if byte == 0x0A:
                event = self._finish_line()
                if event is not None:
                    events.append(event)
                continue

            if self._overlong:
                continue
            if len(self._line) >= MAX_MACHINE_FRAME_BYTES - 1:
                self._overlong = True
                continue
            if not self._line:
                self._start_byte(byte)
            else:
                self._line.append(byte)
                prefix_length = min(len(self._line), len(FRAME_PREFIX))
                self._machine_candidate = (
                    bytes(self._line[:prefix_length]) == FRAME_PREFIX[:prefix_length]
                )
        return tuple(events)


def iter_machine_payloads(chunks: Iterable[bytes]) -> Iterable[bytes]:
    """Small convenience adapter used by pure host tests."""

    framer = Framer()
    for chunk in chunks:
        for event in framer.feed(chunk):
            if isinstance(event, MachineFrame):
                yield event.payload
