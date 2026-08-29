from __future__ import annotations

import unittest

from hidbot.framing import (
    FRAME_PREFIX,
    MAX_MACHINE_FRAME_BYTES,
    Framer,
    MachineFrame,
    MachineFrameIssue,
)


class FramingTests(unittest.TestCase):
    def test_partial_and_multiple_machine_lines(self) -> None:
        logs: list[bytes] = []
        framer = Framer(log_sink=logs.append)
        self.assertEqual(framer.feed(b"I (1) boot\n@HID"), ())
        events = framer.feed(b"BOT {\"id\":1}\r\n@HIDBOT {\"id\":2}\n")
        self.assertEqual(
            events,
            (MachineFrame(b'{"id":1}'), MachineFrame(b'{"id":2}')),
        )
        self.assertEqual(logs, [b"I (1) boot"])

    def test_small_lf_machine_frame(self) -> None:
        framer = Framer()
        self.assertEqual(
            framer.feed(FRAME_PREFIX + b"small\n"),
            (MachineFrame(b"small"),),
        )

    def test_small_crlf_machine_frame(self) -> None:
        framer = Framer()
        self.assertEqual(
            framer.feed(FRAME_PREFIX + b"small\r\n"),
            (MachineFrame(b"small"),),
        )

    def test_maximum_logical_frame_fits_after_crlf_translation(self) -> None:
        payload = b"x" * (1023 - len(FRAME_PREFIX) - 1)
        logical = FRAME_PREFIX + payload + b"\n"
        wire = logical[:-1] + b"\r\n"
        self.assertEqual(len(logical), 1023)
        self.assertEqual(len(wire), MAX_MACHINE_FRAME_BYTES)
        self.assertEqual(Framer().feed(wire), (MachineFrame(payload),))

    def test_logical_frame_one_byte_over_bound_is_oversize_after_crlf(self) -> None:
        payload = b"x" * (1024 - len(FRAME_PREFIX) - 1)
        logical = FRAME_PREFIX + payload + b"\n"
        wire = logical[:-1] + b"\r\n"
        self.assertEqual(len(logical), 1024)
        self.assertEqual(len(wire), MAX_MACHINE_FRAME_BYTES + 1)
        self.assertEqual(
            Framer().feed(wire),
            (MachineFrameIssue("oversize"),),
        )

    def test_prefix_must_be_at_beginning_of_line(self) -> None:
        logs: list[bytes] = []
        framer = Framer(log_sink=logs.append)
        self.assertEqual(framer.feed(b"log @HIDBOT {\"id\":1}\n"), ())
        self.assertEqual(logs, [b"log @HIDBOT {\"id\":1}"])

    def test_oversized_machine_line_recovers_at_next_lf(self) -> None:
        framer = Framer()
        oversized = FRAME_PREFIX + b"x" * MAX_MACHINE_FRAME_BYTES
        events = framer.feed(oversized + b"\n@HIDBOT {}\n")
        self.assertEqual(events[0], MachineFrameIssue("oversize"))
        self.assertEqual(events[1], MachineFrame(b"{}"))

    def test_oversized_normal_line_is_discarded_without_machine_issue(self) -> None:
        logs: list[bytes] = []
        framer = Framer(log_sink=logs.append)
        self.assertEqual(framer.feed(b"normal " + b"x" * 4096 + b"\n"), ())
        self.assertEqual(logs, [])

    def test_partial_line_memory_is_bounded(self) -> None:
        framer = Framer()
        framer.feed(FRAME_PREFIX + b"x" * (MAX_MACHINE_FRAME_BYTES * 20))
        self.assertLessEqual(len(framer._line), MAX_MACHINE_FRAME_BYTES - 1)
        self.assertTrue(framer._overlong)


if __name__ == "__main__":
    unittest.main()
