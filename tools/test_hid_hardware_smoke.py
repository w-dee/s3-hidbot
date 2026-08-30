#!/usr/bin/env python3
"""Pure tests for the U5.4.1/U5.4.2 HID observer and F24 smoke layer."""

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
from types import SimpleNamespace
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


class KeyboardSmokeTests(unittest.TestCase):
    candidate = smoke.InputCandidate(
        Path("/dev/input/event0"),
        "s3-hidbot keyboard",
        0x303A,
        0x4008,
        "s3-hidbot",
        0,
        ("key",),
    )

    def make_fakes(
        self,
        *,
        down_state: str = "submitted",
        up_state: str = "submitted",
        events: list[list[smoke.InputEvent]] | None = None,
        release_error: Exception | None = None,
    ) -> tuple[list[str], object, object, object, list[tuple[int, list[int]]]]:
        actions: list[str] = []
        report_calls: list[tuple[int, list[int]]] = []
        event_batches = list(events or [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 0)],
            [],
        ])

        class FakeObserver:
            def __init__(self, path: Path) -> None:
                self.path = path

            def open(self) -> None:
                actions.append("observer_open")

            def drain(self) -> int:
                actions.append("drain")
                return 0

            def wait_events(self, timeout: float) -> list[smoke.InputEvent]:
                del timeout
                actions.append("observe")
                return event_batches.pop(0) if event_batches else []

            def close(self) -> None:
                actions.append("observer_close")

        class FakeTransport:
            def open(self) -> None:
                actions.append("transport_open")

            def close(self) -> None:
                actions.append("transport_close")

        class FakeClient:
            def __init__(self, transport: FakeTransport) -> None:
                self.transport = transport

            def connect(self) -> object:
                actions.append("hello")
                return SimpleNamespace(session="session")

            def keyboard_report(self, modifiers: int, keys: list[int]) -> object:
                report_calls.append((modifiers, list(keys)))
                actions.append("keyboard_down" if keys else "keyboard_up")
                state = down_state if keys else up_state
                return SimpleNamespace(state=state)

            def release_all(self) -> object:
                actions.append("release_all")
                if release_error is not None:
                    raise release_error
                return {"keyboard": "already_up", "mouse": "already_up"}

            def close(self) -> None:
                self.transport.close()

        def transport_factory(port: str, baudrate: int, timeout: float) -> FakeTransport:
            del port, baudrate, timeout
            actions.append("transport_construct")
            return FakeTransport()

        def client_factory(transport: FakeTransport, timeout: float) -> FakeClient:
            del timeout
            actions.append("client_construct")
            return FakeClient(transport)

        return actions, FakeObserver, transport_factory, client_factory, report_calls

    def execute(
        self, **kwargs: object
    ) -> tuple[int, dict[str, object], list[str], list[tuple[int, list[int]]]]:
        actions, observer_factory, transport_factory, client_factory, report_calls = self.make_fakes(**kwargs)
        code, evidence = smoke.run_keyboard_smoke(
            self.candidate,
            serial_port="fixture-port",
            observer_factory=observer_factory,
            transport_factory=transport_factory,
            client_factory=client_factory,
            clock=lambda: 1.0,
        )
        return code, evidence, actions, report_calls

    def test_success_order_and_exact_client_calls(self) -> None:
        code, evidence, actions, report_calls = self.execute()
        self.assertEqual(code, 0)
        self.assertEqual(evidence["status"], "pass")
        self.assertEqual(
            actions,
            [
                "observer_open",
                "drain",
                "transport_construct",
                "transport_open",
                "client_construct",
                "hello",
                "keyboard_down",
                "observe",
                "keyboard_up",
                "observe",
                "observe",
                "release_all",
                "transport_close",
                "observer_close",
            ],
        )
        self.assertEqual(evidence["down_submit"], "submitted")
        self.assertEqual(evidence["up_submit"], "submitted")
        self.assertTrue(evidence["down_observed"])
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(evidence["allowed_repeat_count"], 0)
        self.assertFalse(evidence["event_evidence_truncated"])
        self.assertEqual(report_calls, [(0, [smoke.F24_USAGE]), (0, [])])

    def test_up_wait_allows_f24_repeat_before_release(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 2),
                smoke.InputEvent(1, 3, smoke.EV_SYN, smoke.SYN_REPORT, 0),
                smoke.InputEvent(1, 4, smoke.EV_KEY, smoke.KEY_F24, 0),
                smoke.InputEvent(1, 5, smoke.EV_SYN, smoke.SYN_REPORT, 0),
            ],
            [],
        ]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 0)
        self.assertEqual(evidence["allowed_repeat_count"], 1)
        self.assertFalse(evidence["repeat_limit_exceeded"])
        self.assertEqual(evidence["up_observed"], True)
        self.assertEqual(
            [entry["classification"] for entry in evidence["event_evidence"]],
            ["expected_down", "allowed_repeat", "ignored_syn", "expected_up", "ignored_syn"],
        )
        self.assertEqual(actions.count("keyboard_up"), 1)

    def test_up_repeats_at_limit_without_release_times_out(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, index, smoke.EV_KEY, smoke.KEY_F24, 2)
                for index in range(2, 2 + smoke.MAX_ALLOWED_F24_REPEATS)
            ],
        ]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertFalse(evidence["up_observed"])
        self.assertEqual(evidence["allowed_repeat_count"], smoke.MAX_ALLOWED_F24_REPEATS)
        self.assertFalse(evidence["repeat_limit_exceeded"])
        self.assertIn("timed out", evidence["error"])
        self.assertEqual(actions.count("release_all"), 1)

    def test_up_repeat_over_limit_then_release_records_release_but_fails(self) -> None:
        repeats = [
            smoke.InputEvent(1, index, smoke.EV_KEY, smoke.KEY_F24, 2)
            for index in range(2, 3 + smoke.MAX_ALLOWED_F24_REPEATS)
        ]
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [*repeats, smoke.InputEvent(1, 99, smoke.EV_KEY, smoke.KEY_F24, 0)],
        ]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(
            evidence["allowed_repeat_count"], smoke.MAX_ALLOWED_F24_REPEATS + 1
        )
        self.assertTrue(evidence["repeat_limit_exceeded"])
        self.assertEqual(evidence["repeat_limit_event"]["phase"], "up_wait")
        self.assertEqual(evidence["repeat_limit_event"]["value"], 2)
        self.assertEqual(
            evidence["repeat_limit_event"]["classification"], "repeat_limit_exceeded"
        )
        self.assertIn("autorepeat limit exceeded", evidence["error"])
        self.assertEqual(actions.count("release_all"), 1)

    def test_up_repeat_over_limit_without_release_fails(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, index, smoke.EV_KEY, smoke.KEY_F24, 2)
                for index in range(2, 3 + smoke.MAX_ALLOWED_F24_REPEATS)
            ],
        ]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertFalse(evidence["up_observed"])
        self.assertEqual(
            evidence["allowed_repeat_count"], smoke.MAX_ALLOWED_F24_REPEATS + 1
        )
        self.assertTrue(evidence["repeat_limit_exceeded"])
        self.assertIn("autorepeat limit exceeded", evidence["error"])
        self.assertEqual(actions.count("release_all"), 1)

    def test_other_key_before_release_fails_with_event_evidence(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, 2, smoke.EV_KEY, 30, 1),
                smoke.InputEvent(1, 3, smoke.EV_KEY, smoke.KEY_F24, 0),
            ],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(evidence["unexpected_event"]["code"], 30)
        self.assertEqual(evidence["unexpected_event"]["phase"], "up_wait")

    def test_f24_press_during_up_wait_fails_closed(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 1)],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertFalse(evidence["up_observed"])
        self.assertEqual(evidence["unexpected_event"]["value"], 1)

    def test_release_then_other_key_same_batch_records_release_but_fails(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 0),
                smoke.InputEvent(1, 3, smoke.EV_KEY, 30, 1),
            ],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(evidence["unexpected_event"]["index"], 1)
        self.assertEqual(
            [entry["classification"] for entry in evidence["event_evidence"]],
            ["expected_down", "expected_up", "unexpected"],
        )

    def test_release_then_repeat_same_batch_fails(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 0),
                smoke.InputEvent(1, 3, smoke.EV_KEY, smoke.KEY_F24, 2),
            ],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(evidence["unexpected_event"]["value"], 2)

    def test_syn_dropped_fails_with_event_evidence(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [smoke.InputEvent(1, 2, smoke.EV_SYN, smoke.SYN_DROPPED, 0)],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertFalse(evidence["up_observed"])
        self.assertEqual(evidence["event_evidence"][1]["classification"], "syn_dropped")

    def test_down_repeat_remains_strict(self) -> None:
        events = [[smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 2)]]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertEqual(evidence["event_evidence"][0]["classification"], "unexpected")

    def test_up_quiet_tail_repeat_fails(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [smoke.InputEvent(1, 2, smoke.EV_KEY, smoke.KEY_F24, 0)],
            [smoke.InputEvent(1, 3, smoke.EV_KEY, smoke.KEY_F24, 2)],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertTrue(evidence["up_observed"])
        self.assertEqual(evidence["unexpected_event"]["phase"], "up_tail")

    def test_event_evidence_is_bounded_and_marks_truncation(self) -> None:
        down_events = [
            smoke.InputEvent(1, index, smoke.EV_SYN, smoke.SYN_REPORT, 0)
            for index in range(smoke.MAX_EVENT_EVIDENCE + 1)
        ]
        down_events.append(smoke.InputEvent(1, 99, smoke.EV_KEY, smoke.KEY_F24, 1))
        events = [
            down_events,
            [smoke.InputEvent(1, 100, smoke.EV_KEY, smoke.KEY_F24, 0)],
            [],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 0)
        self.assertEqual(len(evidence["event_evidence"]), smoke.MAX_EVENT_EVIDENCE)
        self.assertTrue(evidence["event_evidence_truncated"])
        self.assertEqual(evidence["allowed_repeat_count"], 0)
        self.assertFalse(evidence["repeat_limit_exceeded"])
        self.assertEqual(evidence["event_evidence"][0]["index"], 0)
        self.assertEqual(
            evidence["event_evidence"][-1]["index"], smoke.MAX_EVENT_EVIDENCE - 1
        )

    def test_up_repeats_at_limit_then_release_passes(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [
                smoke.InputEvent(1, index, smoke.EV_KEY, smoke.KEY_F24, 2)
                for index in range(2, 2 + smoke.MAX_ALLOWED_F24_REPEATS)
            ]
            + [
                smoke.InputEvent(
                    1,
                    smoke.MAX_ALLOWED_F24_REPEATS + 2,
                    smoke.EV_KEY,
                    smoke.KEY_F24,
                    0,
                ),
            ],
            [],
        ]
        code, evidence, _, _ = self.execute(events=events)
        self.assertEqual(code, 0)
        self.assertEqual(evidence["allowed_repeat_count"], smoke.MAX_ALLOWED_F24_REPEATS)
        self.assertFalse(evidence["repeat_limit_exceeded"])

    def test_no_hardware_keyboard_flag_does_not_discover_or_construct(self) -> None:
        discovery = mock.Mock(side_effect=AssertionError("must not discover"))
        transport = mock.Mock(side_effect=AssertionError("must not construct"))
        result = smoke.main(
            ["--keyboard"],
            discovery_fn=discovery,
            transport_factory=transport,
            errors=io.StringIO(),
        )
        self.assertEqual(result, 2)
        discovery.assert_not_called()
        transport.assert_not_called()

    def test_keyboard_mode_uses_keyboard_only_discovery(self) -> None:
        candidate = self.candidate
        keyboard_discovery = mock.Mock(return_value=candidate)
        actions, observer_factory, transport_factory, client_factory, _ = self.make_fakes()
        code = smoke.main(
            ["--hardware", "--keyboard", "--port", "fixture-port", "--json"],
            discovery_fn=mock.Mock(side_effect=AssertionError("mouse discovery")),
            keyboard_discovery_fn=keyboard_discovery,
            observer_factory=observer_factory,
            transport_factory=transport_factory,
            client_factory=client_factory,
            clock=lambda: 1.0,
            output=io.StringIO(),
            errors=io.StringIO(),
        )
        self.assertEqual(code, 0)
        keyboard_discovery.assert_called_once()
        self.assertEqual(actions.count("transport_construct"), 1)

    def test_down_submit_failure_does_not_wait_or_send_up(self) -> None:
        code, evidence, actions, _ = self.execute(down_state="already_set")
        self.assertEqual(code, 5)
        self.assertEqual(evidence["cleanup_attempted"], True)
        self.assertEqual(actions.count("observe"), 0)
        self.assertEqual(actions.count("keyboard_up"), 0)
        self.assertEqual(actions.count("release_all"), 1)

    def test_down_timeout_attempts_explicit_up_then_cleanup(self) -> None:
        code, evidence, actions, _ = self.execute(events=[[]])
        self.assertEqual(code, 5)
        self.assertEqual(actions.count("keyboard_down"), 1)
        self.assertEqual(actions.count("keyboard_up"), 1)
        self.assertEqual(actions.count("release_all"), 1)

    def test_up_submit_failure_still_cleans_up(self) -> None:
        code, evidence, actions, _ = self.execute(up_state="already_set")
        self.assertEqual(code, 5)
        self.assertEqual(actions.count("keyboard_down"), 1)
        self.assertEqual(actions.count("keyboard_up"), 1)
        self.assertEqual(actions.count("release_all"), 1)

    def test_up_timeout_still_cleans_up(self) -> None:
        events = [
            [smoke.InputEvent(1, 1, smoke.EV_KEY, smoke.KEY_F24, 1)],
            [],
            [],
        ]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertEqual(actions.count("keyboard_down"), 1)
        self.assertEqual(actions.count("keyboard_up"), 1)
        self.assertEqual(actions.count("release_all"), 1)

    def test_syn_dropped_fails_and_cleans_up(self) -> None:
        events = [[smoke.InputEvent(1, 1, smoke.EV_SYN, smoke.SYN_DROPPED, 0)]]
        code, evidence, actions, _ = self.execute(events=events)
        self.assertEqual(code, 5)
        self.assertEqual(actions.count("release_all"), 1)

    def test_cleanup_only_failure_has_distinct_code(self) -> None:
        code, evidence, actions, _ = self.execute(release_error=RuntimeError("cleanup"))
        self.assertEqual(code, 6)
        self.assertEqual(evidence["status"], "fail")
        self.assertIn("cleanup_error", evidence)
        self.assertEqual(actions.count("release_all"), 1)


if __name__ == "__main__":
    unittest.main()
