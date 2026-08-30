#!/usr/bin/env python3
"""Pure tests for the U5.4.1 read-only HID observer/discovery layer."""

from __future__ import annotations

import errno
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/hid_hardware_smoke.py"
SPEC = importlib.util.spec_from_file_location("hid_hardware_smoke", MODULE_PATH)
assert SPEC and SPEC.loader
smoke = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = smoke
SPEC.loader.exec_module(smoke)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ObserverTests(unittest.TestCase):
    def test_decoder_handles_partial_native_records(self) -> None:
        decoder = smoke.InputEventDecoder()
        record = struct.pack("@llHHi", 1, 2, smoke.EV_KEY, smoke.KEY_F24, 1)
        self.assertEqual(decoder.feed(record[:5]), [])
        events = decoder.feed(record[5:])
        self.assertEqual(events, [smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 1)])
        self.assertEqual(decoder.pending_bytes, 0)

    def test_decoder_handles_all_sentinel_event_kinds_and_multiple_records(self) -> None:
        records = b"".join(
            struct.pack("@llHHi", 1, index, event_type, code, value)
            for index, event_type, code, value in (
                (1, smoke.EV_KEY, smoke.KEY_F24, 1),
                (2, smoke.EV_KEY, smoke.KEY_F24, 0),
                (3, smoke.EV_REL, smoke.REL_X, 1),
                (4, smoke.EV_SYN, smoke.SYN_REPORT, 0),
                (5, smoke.EV_SYN, smoke.SYN_DROPPED, 0),
            )
        )
        events = smoke.InputEventDecoder().feed(records)
        self.assertTrue(smoke.is_f24_event(events[0], 1))
        self.assertTrue(smoke.is_f24_event(events[1], 0))
        self.assertTrue(smoke.is_rel_x_event(events[2], 1))
        self.assertFalse(smoke.is_syn_dropped(events[3]))
        self.assertTrue(smoke.is_syn_dropped(events[4]))

    def test_capability_bitset_parser_keeps_multiple_words_in_linux_order(self) -> None:
        self.assertEqual(smoke.parse_capability_bitset("00000000 00000004"), 4)
        self.assertEqual(smoke.parse_capability_bitset("0000 0000 0000"), 0)

    def test_observer_uses_read_only_nonblocking_fd_and_drains(self) -> None:
        observer = smoke.ReadOnlyEventObserver(Path("/dev/input/event7"))
        with mock.patch.object(smoke.os, "open", return_value=41) as opened, mock.patch.object(
            smoke.os, "read", side_effect=BlockingIOError(errno.EAGAIN, "again")
        ) as read, mock.patch.object(smoke.os, "close"):
            observer.open()
            self.assertEqual(observer.drain(), 0)
        flags = opened.call_args.args[1]
        self.assertFalse(flags & os.O_WRONLY)
        self.assertFalse(flags & os.O_RDWR)
        self.assertTrue(flags & os.O_NONBLOCK)
        if hasattr(os, "O_CLOEXEC"):
            self.assertTrue(flags & os.O_CLOEXEC)
        read.assert_called_once_with(41, 4096)

    def test_observer_rejects_malformed_partial_record(self) -> None:
        observer = smoke.ReadOnlyEventObserver(Path("/dev/input/event7"))
        record = struct.pack("@llHHi", 1, 2, smoke.EV_KEY, smoke.KEY_F24, 1)
        with mock.patch.object(smoke.os, "open", return_value=41), mock.patch.object(
            smoke.os,
            "read",
            side_effect=[record[:1], BlockingIOError(errno.EAGAIN, "again")],
        ), mock.patch.object(smoke.os, "close"):
            observer.open()
            with self.assertRaises(smoke.ObserverError):
                observer.drain()

    def test_observer_rejects_syn_dropped(self) -> None:
        observer = smoke.ReadOnlyEventObserver(Path("/dev/input/event7"))
        record = struct.pack("@llHHi", 1, 2, smoke.EV_SYN, smoke.SYN_DROPPED, 0)
        with mock.patch.object(smoke.os, "open", return_value=41), mock.patch.object(
            smoke.os,
            "read",
            side_effect=[record, BlockingIOError(errno.EAGAIN, "again")],
        ), mock.patch.object(smoke.os, "close"):
            observer.open()
            with self.assertRaises(smoke.ObserverError):
                observer.drain()


class DiscoveryTests(unittest.TestCase):
    def _fixture(self) -> tuple[Path, Path]:
        temporary = Path(self.tempdir.name)
        dev = temporary / "dev/input"
        sysfs = temporary / "sys"
        for number, interface, group, bit, label in (
            (0, "00", "key", smoke.KEY_F24, "keyboard"),
            (1, "01", "rel", smoke.REL_X, "mouse"),
        ):
            usb = temporary / f"devices/usb/1-1/{label}"
            interface_node = usb / f"1-1:1.{number}"
            input_node = interface_node / "input" / f"input{number}"
            event_node = input_node / f"event{number}"
            event_node.mkdir(parents=True)
            _write(usb / "idVendor", "303a\n")
            _write(usb / "idProduct", "4008\n")
            _write(usb / "product", "s3-hidbot\n")
            _write(interface_node / "bInterfaceNumber", f"{interface}\n")
            _write(input_node / "name", f"s3-hidbot {label}\n")
            _write(input_node / "capabilities" / group, f"{1 << bit:x}\n")
            (sysfs / "class/input").mkdir(parents=True, exist_ok=True)
            (sysfs / "class/input" / f"event{number}").symlink_to(event_node)
            dev.mkdir(parents=True, exist_ok=True)
            (dev / f"event{number}").touch()
        return dev, sysfs

    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _fresh_fixture(self) -> tuple[Path, Path]:
        self.tempdir.cleanup()
        self.tempdir = tempfile.TemporaryDirectory()
        return self._fixture()

    def test_discovery_validates_identity_interface_and_capability(self) -> None:
        dev, sysfs = self._fixture()
        keyboard, mouse = smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)
        self.assertEqual(keyboard.interface_number, 0)
        self.assertEqual(mouse.interface_number, 1)
        self.assertIn("key", keyboard.capabilities)
        self.assertIn("rel", mouse.capabilities)

    def test_ambiguous_candidates_fail_closed(self) -> None:
        dev, sysfs = self._fixture()
        duplicate = dev / "event2"
        duplicate.touch()
        original = (sysfs / "class/input/event0").resolve()
        (sysfs / "class/input/event2").symlink_to(original)
        with self.assertRaises(smoke.DiscoveryError):
            smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)

    def test_wrong_identity_interface_and_capability_fail_closed(self) -> None:
        cases = (
            ("devices/usb/1-1/keyboard/idVendor", "1234\n"),
            ("devices/usb/1-1/keyboard/idProduct", "5678\n"),
            ("devices/usb/1-1/keyboard/product", "other\n"),
            ("devices/usb/1-1/keyboard/1-1:1.0/bInterfaceNumber", "09\n"),
        )
        for relative, value in cases:
            with self.subTest(relative=relative):
                dev, sysfs = self._fresh_fixture()
                _write(Path(self.tempdir.name) / relative, value)
                with self.assertRaises(smoke.DiscoveryError):
                    smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)

        dev, sysfs = self._fresh_fixture()
        (
            Path(self.tempdir.name)
            / "devices/usb/1-1/keyboard/1-1:1.0/input/input0/capabilities/key"
        ).unlink()
        with self.assertRaises(smoke.DiscoveryError):
            smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)

    def test_zero_candidate_and_ambiguous_mouse_fail_closed(self) -> None:
        dev, sysfs = self._fresh_fixture()
        _write(Path(self.tempdir.name) / "devices/usb/1-1/keyboard/product", "other\n")
        with self.assertRaises(smoke.DiscoveryError):
            smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)

        dev, sysfs = self._fresh_fixture()
        duplicate = dev / "event2"
        duplicate.touch()
        original = (sysfs / "class/input/event1").resolve()
        (sysfs / "class/input/event2").symlink_to(original)
        with self.assertRaises(smoke.DiscoveryError):
            smoke.discover_devices(dev_input_root=dev, sysfs_root=sysfs)


class SafetyTests(unittest.TestCase):
    def test_without_hardware_flag_does_not_discover_or_open(self) -> None:
        output = io.StringIO()
        errors = io.StringIO()
        discovery = mock.Mock(side_effect=AssertionError("discovery must not run"))
        result = smoke.main([], discovery_fn=discovery, output=output, errors=errors)
        self.assertEqual(result, 2)
        discovery.assert_not_called()
        self.assertIn("--hardware", errors.getvalue())

    def test_non_linux_is_fail_closed_before_discovery(self) -> None:
        discovery = mock.Mock(side_effect=AssertionError("discovery must not run"))
        result = smoke.main(
            ["--hardware"],
            platform_name="darwin",
            discovery_fn=discovery,
            errors=io.StringIO(),
        )
        self.assertEqual(result, 2)
        discovery.assert_not_called()

    def test_hardware_mode_only_observes_and_reports_no_hid_request(self) -> None:
        candidates = (
            smoke.InputCandidate(Path("/dev/input/event0"), "kbd", 0x303A, 0x4008, "s3-hidbot", 0, ("key",)),
            smoke.InputCandidate(Path("/dev/input/event1"), "mouse", 0x303A, 0x4008, "s3-hidbot", 1, ("rel",)),
        )

        class FakeObserver:
            opened: list[Path] = []
            closed: list[Path] = []

            def __init__(self, path: Path) -> None:
                self.path = path

            def open(self) -> None:
                self.opened.append(self.path)

            def drain(self) -> int:
                return 0

            def close(self) -> None:
                self.closed.append(self.path)

        output = io.StringIO()
        result = smoke.main(
            ["--hardware", "--json"],
            discovery_fn=mock.Mock(return_value=candidates),
            observer_factory=FakeObserver,
            output=output,
            errors=io.StringIO(),
        )
        self.assertEqual(result, 0)
        self.assertEqual(FakeObserver.opened, [candidate.path for candidate in candidates])
        self.assertEqual(FakeObserver.closed, [candidate.path for candidate in candidates])
        self.assertIn('"hid_reports_sent": 0', output.getvalue())
        self.assertIn('"observer_opened": true', output.getvalue())
        self.assertIn('"serial_accessed": false', output.getvalue())


if __name__ == "__main__":
    unittest.main()
