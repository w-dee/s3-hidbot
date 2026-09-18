#!/usr/bin/env python3
"""Deterministic regression tests for BLE qualification cleanup authority."""

from __future__ import annotations

import errno
import ast
from pathlib import Path
import unittest
from unittest import mock
from types import SimpleNamespace

import ble_cleanup_rehearsal as rehearsal

from qualification_harness import (
    BleCleanupPhase,
    FinalBleControlState,
    ObserverTerminalStatus,
    QualificationError,
    classify_observer_error,
    observation_complete,
    run_ble_cleanup,
)


class SessionLost(RuntimeError):
    pass


class Session:
    def __init__(self, label: str, actions: list[str]) -> None:
        self.label = label
        self.actions = actions
        self.session = "attempt-session"
        self.boot_id = "attempt-boot"

    def close(self) -> None:
        self.actions.append("close:" + self.label)


class CleanupFixture:
    def __init__(self) -> None:
        self.actions: list[str] = []
        self.session = Session("attempt", self.actions)
        self.all_up = observation_complete()
        self.quiet = observation_complete()
        self.retirement = observation_complete()
        self.final_reads = 0
        self.release_calls = 0
        self.retirement_calls = 0
        self.lose_first_cleanup_session = False
        self.lose_first_retirement_session = False
        self.lose_first_final_session = False

    def ping(self, session: Session) -> None:
        self.actions.append("ping:" + session.label)

    def intent(self, operation: str) -> None:
        self.actions.append("intent:" + operation)

    def release(self, session: Session) -> dict[str, str]:
        self.release_calls += 1
        self.actions.append("release:" + session.label)
        if self.lose_first_cleanup_session and self.release_calls == 1:
            raise SessionLost("mutation may already have executed")
        return {"keyboard": "already_up", "mouse": "already_up"}

    def observe_all_up(self):
        self.actions.append("observe_all_up")
        return self.all_up

    def quiet_tail(self):
        self.actions.append("quiet_tail")
        return self.quiet

    def retire(self, session: Session) -> None:
        self.retirement_calls += 1
        self.actions.append("retire:" + session.label)
        if self.lose_first_retirement_session and self.retirement_calls == 1:
            raise SessionLost("retirement may already have executed")

    def observer_retirement(self):
        self.actions.append("observer_retirement")
        return self.retirement

    def acquire_retired(self) -> Session:
        self.actions.append("acquire:post_retirement")
        return Session("post_retirement", self.actions)

    def terminal(self, session: Session) -> None:
        self.actions.append("terminal:" + session.label)

    def final_state(self, session: Session) -> FinalBleControlState:
        self.final_reads += 1
        self.actions.append("final:" + session.label)
        if self.lose_first_final_session and self.final_reads == 1:
            raise SessionLost("expired")
        return FinalBleControlState(True, False, "ALL_UP", False, "hidden_idle_disconnected")

    def run(self):
        return run_ble_cleanup(
            record_mutation_intent=self.intent,
            attempt_session=self.session,
            session_identity=lambda client: (client.boot_id, client.session),
            keep_session_alive=self.ping,
            release_all=self.release,
            observe_all_up=self.observe_all_up,
            observe_quiet_tail=self.quiet_tail,
            request_retirement=self.retire,
            observe_retirement=self.observer_retirement,
            acquire_retired_session=self.acquire_retired,
            request_ble_terminal=self.terminal,
            read_final_state=self.final_state,
            required_ble_terminal="hidden_idle_disconnected",
        )


class BleCleanupQualificationTests(unittest.TestCase):
    def test_same_workload_session_is_used_until_retirement(self) -> None:
        fixture = CleanupFixture()
        result = fixture.run()
        self.assertEqual(result["status"], "pass")
        self.assertTrue(result["same_attempt_session"])
        for prefix in ("release:", "retire:"):
            self.assertIn(prefix + "attempt", fixture.actions)
        self.assertEqual([action for action in fixture.actions if action.startswith("acquire:")],
                         ["acquire:post_retirement"])
        self.assertGreater(fixture.actions.index("acquire:post_retirement"),
                           fixture.actions.index("retire:attempt"))

    def test_session_loss_after_release_side_effect_is_not_replayed(self) -> None:
        fixture = CleanupFixture()
        fixture.lose_first_cleanup_session = True
        with self.assertRaisesRegex(QualificationError, "no replay"):
            fixture.run()
        self.assertEqual(fixture.release_calls, 1)
        self.assertEqual(fixture.retirement_calls, 0)

    def test_session_identity_change_during_quiet_tail_fails(self) -> None:
        fixture = CleanupFixture()
        def quiet():
            fixture.session.session = "replacement"
            return observation_complete()
        fixture.quiet_tail = quiet
        with self.assertRaisesRegex(QualificationError, "continuity lost"):
            fixture.run()
        self.assertEqual(fixture.retirement_calls, 0)

    def test_lease_loss_without_local_identity_change_fails(self) -> None:
        fixture = CleanupFixture()
        lost = [False]
        def ping(session):
            if lost[0]:
                raise SessionLost("expired")
        def quiet_with_loss():
            lost[0] = True
            return observation_complete()
        fixture.ping = ping
        fixture.quiet_tail = quiet_with_loss
        with self.assertRaisesRegex(QualificationError, "continuity lost"):
            fixture.run()
        self.assertEqual(fixture.retirement_calls, 0)

    def test_r2_observer_loss_before_all_up_is_unexpected(self) -> None:
        fixture = CleanupFixture()
        fixture.all_up = classify_observer_error(
            OSError(errno.ENODEV, "gone"),
            phase=BleCleanupPhase.PRE_RETIREMENT_CLEANUP,
            exact_device=True,
            retirement_requested=False,
        )
        self.assertEqual(
            fixture.all_up.status, ObserverTerminalStatus.UNEXPECTED_DEVICE_LOSS
        )
        with self.assertRaisesRegex(QualificationError, "before ALL_UP"):
            fixture.run()
        self.assertFalse(any(action.startswith("retire:") for action in fixture.actions))

    def test_r3_observer_loss_during_quiet_tail_fails(self) -> None:
        fixture = CleanupFixture()
        fixture.quiet = classify_observer_error(
            OSError(errno.ENOENT, "gone"),
            phase=BleCleanupPhase.ALL_UP_PROVEN,
            exact_device=True,
            retirement_requested=False,
        )
        with self.assertRaisesRegex(QualificationError, "before retirement"):
            fixture.run()
        self.assertFalse(any(action.startswith("retire:") for action in fixture.actions))

    def test_r4_exact_device_loss_after_boundary_is_correlated(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            OSError(errno.ENODEV, "gone"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=True,
            retirement_requested=True,
        )
        result = fixture.run()
        self.assertEqual(
            result["observer_retirement"]["status"],
            ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED,
        )
        self.assertTrue(result["final_control_state"]["route_none"])

    def test_r4_retirement_without_exact_identity_fails(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            OSError(errno.ENODEV, "gone"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=False,
            retirement_requested=True,
        )
        with self.assertRaisesRegex(QualificationError, "retirement evidence"):
            fixture.run()

    def test_r5_arbitrary_oserror_after_boundary_fails(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            OSError(errno.EIO, "io"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=True,
            retirement_requested=True,
        )
        self.assertEqual(fixture.retirement.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        with self.assertRaisesRegex(QualificationError, "retirement evidence"):
            fixture.run()

    def test_r6_route_none_cannot_replace_all_up_proof(self) -> None:
        fixture = CleanupFixture()
        fixture.all_up = observation_complete(held_keys=1)
        with self.assertRaisesRegex(QualificationError, "ALL_UP"):
            fixture.run()
        self.assertFalse(any(action.startswith("retire:") for action in fixture.actions))

    def test_r7_successful_full_cleanup_has_exact_phase_order(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            FileNotFoundError(errno.ENOENT, "gone"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=True,
            retirement_requested=True,
        )
        result = fixture.run()
        self.assertEqual(
            result["phases"],
            [phase.value for phase in BleCleanupPhase],
        )
        self.assertTrue(result["same_attempt_session"])
        self.assertLess(fixture.actions.index("quiet_tail"), fixture.actions.index("retire:attempt"))
        self.assertLess(
            fixture.actions.index("intent:route_retirement"),
            fixture.actions.index("retire:attempt"),
        )
        self.assertLess(
            fixture.actions.index("intent:ble_terminal"),
            fixture.actions.index("terminal:post_retirement"),
        )

    def test_r8_observer_disappearance_is_not_required(self) -> None:
        fixture = CleanupFixture()
        result = fixture.run()
        self.assertEqual(
            result["observer_retirement"]["status"],
            ObserverTerminalStatus.OBSERVATION_COMPLETE,
        )

    def test_final_session_loss_fails_without_reacquisition(self) -> None:
        fixture = CleanupFixture()
        fixture.lose_first_final_session = True
        with self.assertRaisesRegex(QualificationError, "no replay"):
            fixture.run()
        self.assertEqual(fixture.final_reads, 1)

    def test_ambiguous_retirement_is_not_replayed(self) -> None:
        fixture = CleanupFixture()
        fixture.lose_first_retirement_session = True
        with self.assertRaisesRegex(QualificationError, "no replay"):
            fixture.run()
        self.assertEqual(fixture.retirement_calls, 1)
        self.assertNotIn("observer_retirement", fixture.actions)

    def test_quiet_tail_requires_exact_device(self) -> None:
        from dataclasses import replace
        fixture = CleanupFixture()
        fixture.quiet = replace(observation_complete(), exact_device=False)
        with self.assertRaisesRegex(QualificationError, "exact device"):
            fixture.run()

    def test_final_state_must_correlate_expected_retirement(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            OSError(errno.ENODEV, "gone"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=True,
            retirement_requested=True,
        )
        fixture.final_state = lambda session: FinalBleControlState(
            False, False, "ALL_UP", False, "hidden_idle_disconnected"
        )
        with self.assertRaisesRegex(QualificationError, "not safely retired"):
            fixture.run()

    def test_expected_retirement_requires_final_disconnection(self) -> None:
        fixture = CleanupFixture()
        fixture.retirement = classify_observer_error(
            OSError(errno.ENODEV, "gone"),
            phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
            exact_device=True,
            retirement_requested=True,
        )
        fixture.final_state = lambda session: FinalBleControlState(
            True, False, "ALL_UP", True, "hidden_idle_disconnected"
        )
        with self.assertRaisesRegex(QualificationError, "not safely retired"):
            fixture.run()

    def test_hardware_rehearsal_has_no_normal_hid_workload_call(self) -> None:
        source = Path(__file__).with_name("ble_cleanup_rehearsal.py").read_text(
            encoding="utf-8"
        )
        tree = ast.parse(source)
        invoked = {
            node.func.attr
            for node in ast.walk(tree)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
        }
        self.assertTrue(
            invoked.isdisjoint({"sequence_start", "keyboard_report", "mouse_report"})
        )
        self.assertNotIn("SequenceBuilder", source)


class PhysicalRunnerAdapterTests(unittest.TestCase):
    @staticmethod
    def event(event_type: int, code: int, value: int) -> rehearsal.Event:
        return rehearsal.Event((0, 0, event_type, code, value))

    def observer_outcome(
        self,
        keyboard_reads,
        mouse_reads,
        *,
        phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
        retirement=True,
        keyboard_held=0,
        mouse_held=0,
    ):
        keyboard_queue = list(keyboard_reads)
        mouse_queue = list(mouse_reads)
        def next_read(queue):
            item = queue.pop(0) if queue else []
            if isinstance(item, BaseException):
                raise item
            return item
        keyboard = mock.Mock(
            role="keyboard",
            held_count=mock.Mock(return_value=keyboard_held),
            finish_pending=mock.Mock(return_value=[]),
        )
        keyboard.read.side_effect = lambda _timeout: next_read(keyboard_queue)
        mouse = mock.Mock(
            role="mouse",
            held_count=mock.Mock(return_value=mouse_held),
            finish_pending=mock.Mock(return_value=[]),
        )
        mouse.read.side_effect = lambda _timeout: next_read(mouse_queue)
        clock = iter(index * 0.05 for index in range(1000))
        with mock.patch.object(rehearsal.time, "monotonic", side_effect=clock):
            return rehearsal.ExactObservers(keyboard, mouse).collect(
                1, phase=phase, retirement=retirement
            )

    def raw_observer_outcome(
        self,
        keyboard_reads,
        mouse_reads,
        *,
        phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
        retirement=True,
    ):
        keyboard = rehearsal.Observer(Path("/keyboard"), "keyboard")
        mouse = rehearsal.Observer(Path("/mouse"), "mouse")
        keyboard.fd = 101
        mouse.fd = 102
        queues = {101: list(keyboard_reads), 102: list(mouse_reads)}

        def read(fd, _length):
            item = queues[fd].pop(0) if queues[fd] else BlockingIOError(
                errno.EAGAIN, "drained"
            )
            if isinstance(item, BaseException):
                raise item
            return item

        selector = mock.MagicMock()
        selector.__enter__.return_value.select.return_value = [(object(), 1)]
        clock = iter(index * 0.05 for index in range(1000))
        with mock.patch.object(rehearsal.selectors, "DefaultSelector",
                               return_value=selector), \
             mock.patch.object(rehearsal.os, "read", side_effect=read), \
             mock.patch.object(rehearsal.fcntl, "ioctl", return_value=0), \
             mock.patch.object(rehearsal.time, "monotonic", side_effect=clock):
            return rehearsal.ExactObservers(keyboard, mouse).collect(
                1, phase=phase, retirement=retirement
            )

    @staticmethod
    def raw(event_type: int, code: int, value: int) -> bytes:
        return rehearsal.Event.record.pack(0, 0, event_type, code, value)

    def test_raw_unexpected_keyboard_survives_enodev(self) -> None:
        outcome = self.raw_observer_outcome(
            [self.raw(rehearsal.EV_KEY, 30, 1), OSError(errno.ENODEV, "gone")],
            [],
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual(outcome.unexpected_events, 1)

    def test_raw_unexpected_mouse_survives_enodev(self) -> None:
        outcome = self.raw_observer_outcome(
            [BlockingIOError(errno.EAGAIN, "drained"),
             BlockingIOError(errno.EAGAIN, "drained")],
            [self.raw(rehearsal.EV_REL, rehearsal.REL_X, 1),
             OSError(errno.ENODEV, "gone")],
        )
        self.assertEqual(outcome.unexpected_events, 1)

    def test_raw_held_key_survives_enodev(self) -> None:
        outcome = self.raw_observer_outcome(
            [self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1),
             OSError(errno.ENODEV, "gone")],
            [],
        )
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (1, 1))

    def test_multiple_raw_events_are_owned_before_enodev(self) -> None:
        records = (
            self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
            + self.raw(rehearsal.EV_KEY, 30, 1)
        )
        outcome = self.raw_observer_outcome(
            [records, OSError(errno.ENODEV, "gone")], []
        )
        self.assertEqual(
            (outcome.relevant_events, outcome.unexpected_events,
             outcome.held_keys),
            (1, 1, 1),
        )

    def test_clean_complete_raw_events_allow_expected_retirement(self) -> None:
        records = (
            self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
            + self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 0)
        )
        outcome = self.raw_observer_outcome(
            [records, OSError(errno.ENODEV, "gone")], []
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (2, 0))

    def test_complete_raw_event_plus_partial_record_is_io_error(self) -> None:
        record = self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
        outcome = self.raw_observer_outcome(
            [record + record[:5], OSError(errno.ENODEV, "gone")], []
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (1, 1))

    def test_pending_syn_dropped_survives_other_observer_retirement(self) -> None:
        keyboard = (
            self.raw(rehearsal.EV_SYN, 0, 0)
            + self.raw(rehearsal.EV_SYN, rehearsal.SYN_DROPPED, 0)
        )
        outcome = self.raw_observer_outcome(
            [keyboard], [OSError(errno.ENODEV, "mouse gone")]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual(outcome.error_name, "EIO")

    def test_complete_records_before_pending_syn_dropped_remain_aggregated(self) -> None:
        keyboard = (
            self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
            + self.raw(rehearsal.EV_KEY, 30, 1)
            + self.raw(rehearsal.EV_SYN, rehearsal.SYN_DROPPED, 0)
        )
        outcome = self.raw_observer_outcome(
            [keyboard], [OSError(errno.ENODEV, "mouse gone")]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual(
            (outcome.relevant_events, outcome.unexpected_events,
             outcome.held_keys),
            (1, 1, 1),
        )

    def test_pending_partial_survives_other_observer_retirement(self) -> None:
        record = self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
        outcome = self.raw_observer_outcome(
            [record + record[:5]], [OSError(errno.ENODEV, "mouse gone")]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (1, 1))

    def test_partial_only_eagain_fails_before_deadline(self) -> None:
        record = self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
        outcome = self.raw_observer_outcome(
            [record[:5], BlockingIOError(errno.EAGAIN, "drained")], []
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual(outcome.error_name, "EIO")

    def test_mouse_pending_anomaly_survives_keyboard_retirement(self) -> None:
        mouse = (
            self.raw(rehearsal.EV_SYN, 0, 0)
            + self.raw(rehearsal.EV_SYN, rehearsal.SYN_DROPPED, 0)
        )
        outcome = self.raw_observer_outcome(
            [BlockingIOError(errno.EAGAIN, "drained"),
             OSError(errno.ENODEV, "keyboard gone")],
            [mouse],
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual(outcome.error_name, "EIO")

    def test_unapproved_error_cannot_hide_behind_other_retirement(self) -> None:
        for first, second in ((errno.ENODEV, errno.ENXIO),
                              (errno.ENXIO, errno.ENODEV)):
            outcome = self.raw_observer_outcome(
                [OSError(first, "keyboard")], [OSError(second, "mouse")]
            )
            self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
            self.assertEqual(outcome.error_name, "ENXIO")
            fixture = CleanupFixture()
            fixture.retirement = outcome
            with self.assertRaises(QualificationError):
                fixture.run()

    def test_pending_complete_evidence_survives_partial_tail(self) -> None:
        observer = rehearsal.Observer(Path("/fake"), "keyboard")
        observer.pending.extend(self.raw(rehearsal.EV_KEY, 30, 1) + b"x")
        with self.assertRaises(OSError) as caught:
            observer.finish_pending()
        self.assertEqual(caught.exception.errno, errno.EIO)
        self.assertEqual([(e.code, e.value) for e in getattr(caught.exception, "events", [])], [(30, 1)])

    def test_keyboard_retirement_drains_unread_mouse_button(self) -> None:
        outcome = self.raw_observer_outcome(
            [OSError(errno.ENODEV, "keyboard gone")],
            [self.raw(rehearsal.EV_KEY, rehearsal.BTN_LEFT, 1)],
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual((outcome.relevant_events, outcome.held_buttons), (1, 1))
        fixture = CleanupFixture()
        fixture.retirement = outcome
        with self.assertRaisesRegex(QualificationError, "unexpected held input"):
            fixture.run()

    def test_keyboard_retirement_drains_unread_mouse_syn_dropped(self) -> None:
        outcome = self.raw_observer_outcome(
            [OSError(errno.ENODEV, "keyboard gone")],
            [self.raw(rehearsal.EV_SYN, rehearsal.SYN_DROPPED, 0)],
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual(outcome.error_name, "EIO")

    def test_mouse_retirement_drains_unread_keyboard_records_in_order(self) -> None:
        records = (
            self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
            + self.raw(rehearsal.EV_KEY, 30, 1)
            + self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 0)
        )
        outcome = self.raw_observer_outcome(
            [records], [OSError(errno.ENODEV, "mouse gone")]
        )
        self.assertEqual(
            (outcome.relevant_events, outcome.unexpected_events,
             outcome.held_keys),
            (2, 1, 0),
        )

    def test_retirement_drains_other_complete_record_and_partial_tail(self) -> None:
        record = self.raw(rehearsal.EV_KEY, rehearsal.BTN_LEFT, 1)
        outcome = self.raw_observer_outcome(
            [OSError(errno.ENODEV, "keyboard gone")],
            [record + record[:5]],
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual((outcome.relevant_events, outcome.held_buttons), (1, 1))

    def test_one_retired_other_clean_until_deadline_uses_bounded_policy(self) -> None:
        outcome = self.observer_outcome(
            [OSError(errno.ENODEV, "keyboard gone")], [[]], mouse_held=0
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual((outcome.relevant_events, outcome.held_buttons), (0, 0))

    def test_deadline_edge_nonblocking_drain_owns_new_mouse_event(self) -> None:
        keyboard = mock.Mock(
            role="keyboard", read=mock.Mock(return_value=[]),
            held_count=mock.Mock(return_value=0),
            finish_pending=mock.Mock(return_value=[]),
        )
        mouse = mock.Mock(
            role="mouse",
            read=mock.Mock(side_effect=[
                [self.event(rehearsal.EV_KEY, rehearsal.BTN_LEFT, 1)], []
            ]),
            held_count=mock.Mock(return_value=0),
            finish_pending=mock.Mock(return_value=[]),
        )
        with mock.patch.object(rehearsal.time, "monotonic", side_effect=[0, 2]):
            outcome = rehearsal.ExactObservers(keyboard, mouse).collect(
                1, phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
                retirement=True,
            )
        self.assertEqual((outcome.relevant_events, outcome.held_buttons), (1, 1))

    def test_raw_event_survives_eio(self) -> None:
        outcome = self.raw_observer_outcome(
            [self.raw(rehearsal.EV_KEY, rehearsal.KEY_F24, 1),
             OSError(errno.EIO, "io")], []
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (1, 1))

    def test_raw_event_survives_pre_retirement_enodev(self) -> None:
        outcome = self.raw_observer_outcome(
            [self.raw(rehearsal.EV_KEY, 30, 1), OSError(errno.ENODEV, "gone")],
            [], phase=BleCleanupPhase.PRE_RETIREMENT_CLEANUP,
            retirement=False,
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.UNEXPECTED_DEVICE_LOSS)
        self.assertEqual(outcome.unexpected_events, 1)

    def test_retirement_preserves_unexpected_keyboard_evidence(self) -> None:
        unexpected = self.event(rehearsal.EV_KEY, 30, 1)
        outcome = self.observer_outcome(
            [[unexpected], OSError(errno.ENODEV, "gone")], [[], []]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual(outcome.unexpected_events, 1)
        fixture = CleanupFixture()
        fixture.retirement = outcome
        with self.assertRaisesRegex(QualificationError, "unexpected held input"):
            fixture.run()

    def test_retirement_preserves_held_f24_evidence(self) -> None:
        down = self.event(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
        outcome = self.observer_outcome(
            [[down], OSError(errno.ENODEV, "gone")], [[], []]
        )
        self.assertEqual(outcome.relevant_events, 1)
        self.assertEqual(outcome.held_keys, 1)
        fixture = CleanupFixture()
        fixture.retirement = outcome
        with self.assertRaisesRegex(QualificationError, "unexpected held input"):
            fixture.run()

    def test_clean_retirement_keeps_zero_evidence(self) -> None:
        outcome = self.observer_outcome(
            [OSError(errno.ENODEV, "gone")], [[]]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual(
            (outcome.relevant_events, outcome.unexpected_events,
             outcome.held_keys, outcome.held_buttons),
            (0, 0, 0, 0),
        )

    def test_retirement_preserves_unexpected_mouse_evidence(self) -> None:
        movement = self.event(rehearsal.EV_REL, rehearsal.REL_X, 1)
        outcome = self.observer_outcome(
            [[], []], [[movement], OSError(errno.ENODEV, "gone")]
        )
        self.assertEqual(outcome.unexpected_events, 1)

    def test_final_held_query_failure_preserves_prior_evidence(self) -> None:
        unexpected = self.event(rehearsal.EV_KEY, 30, 1)
        keyboard = mock.Mock(role="keyboard")
        keyboard.read.side_effect = [[unexpected], [], []]
        keyboard.held_count.return_value = 1
        keyboard.finish_pending.return_value = []
        mouse = mock.Mock(role="mouse")
        mouse.read.side_effect = [[], [], []]
        mouse.held_count.side_effect = OSError(errno.ENODEV, "gone")
        mouse.finish_pending.return_value = []
        observer = rehearsal.ExactObservers(keyboard, mouse)
        with mock.patch.object(
            rehearsal.time, "monotonic", side_effect=[0, 0, 0, 0, 2]
        ):
            outcome = observer.collect(
                1,
                phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
                retirement=True,
            )
        self.assertEqual(outcome.status, ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual(outcome.unexpected_events, 1)
        self.assertEqual(outcome.held_keys, 1)

    def test_raw_evidence_survives_final_eviocgkey_failure(self) -> None:
        keyboard = rehearsal.Observer(Path("/keyboard"), "keyboard")
        mouse = rehearsal.Observer(Path("/mouse"), "mouse")
        keyboard.fd = 101
        mouse.fd = 102
        queues = {
            101: [self.raw(rehearsal.EV_KEY, 30, 1),
                  BlockingIOError(errno.EAGAIN, "drained")],
            102: [BlockingIOError(errno.EAGAIN, "drained")],
        }

        def read(fd, _length):
            item = queues[fd].pop(0) if queues[fd] else BlockingIOError(
                errno.EAGAIN, "drained"
            )
            if isinstance(item, BaseException):
                raise item
            return item

        def held(fd, _request, _data, _mutate):
            if fd == 102:
                raise OSError(errno.ENODEV, "gone")

        selector = mock.MagicMock()
        selector.__enter__.return_value.select.return_value = [(object(), 1)]
        with mock.patch.object(rehearsal.selectors, "DefaultSelector",
                               return_value=selector), \
             mock.patch.object(rehearsal.os, "read", side_effect=read), \
             mock.patch.object(rehearsal.fcntl, "ioctl", side_effect=held), \
             mock.patch.object(rehearsal.time, "monotonic",
                               side_effect=[0, 0, 0, 0, 2]):
            outcome = rehearsal.ExactObservers(keyboard, mouse).collect(
                1,
                phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
                retirement=True,
            )
        self.assertEqual(outcome.status,
                         ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED)
        self.assertEqual(outcome.unexpected_events, 1)

    def test_observer_io_error_preserves_prior_evidence(self) -> None:
        down = self.event(rehearsal.EV_KEY, rehearsal.KEY_F24, 1)
        outcome = self.observer_outcome(
            [[down], OSError(errno.EIO, "io")], [[], []]
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.OBSERVER_IO_ERROR)
        self.assertEqual((outcome.relevant_events, outcome.held_keys), (1, 1))

    def test_pre_retirement_loss_preserves_prior_evidence(self) -> None:
        unexpected = self.event(rehearsal.EV_KEY, 30, 1)
        outcome = self.observer_outcome(
            [[unexpected], OSError(errno.ENODEV, "gone")], [[], []],
            phase=BleCleanupPhase.PRE_RETIREMENT_CLEANUP,
            retirement=False,
        )
        self.assertEqual(outcome.status, ObserverTerminalStatus.UNEXPECTED_DEVICE_LOSS)
        self.assertEqual(outcome.unexpected_events, 1)

    def test_all_up_queries_preexisting_kernel_key_state(self) -> None:
        observer = rehearsal.Observer(Path("/unused"), "keyboard")
        observer.fd = 123
        def held(fd, request, data, mutate):
            self.assertEqual(fd, 123)
            self.assertEqual(request, 0x80604518)
            data[24] = 4  # KEY_F24 was already held before opening the observer.
        with mock.patch.object(rehearsal.fcntl, "ioctl", side_effect=held):
            self.assertEqual(observer.held_count(), 1)

    def test_empty_history_cannot_hide_preexisting_held_state(self) -> None:
        keyboard = mock.Mock(read=lambda _timeout: [], held_count=lambda: 1,
                             finish_pending=lambda: [])
        mouse = mock.Mock(read=lambda _timeout: [], held_count=lambda: 0,
                          finish_pending=lambda: [])
        observer = rehearsal.ExactObservers(keyboard, mouse)
        outcome = observer.collect(0, phase=BleCleanupPhase.PRE_RETIREMENT_CLEANUP,
                                   retirement=False)
        self.assertEqual(outcome.held_keys, 1)
        fixture = CleanupFixture()
        fixture.all_up = outcome
        with self.assertRaisesRegex(QualificationError, "ALL_UP"):
            fixture.run()

    def test_real_runner_keeps_route_session_through_cleanup(self) -> None:
        labels = []
        active_route = ["none"]
        hidden = [False]
        def route():
            return SimpleNamespace(desired=SimpleNamespace(value=active_route[0]),
                                   active=SimpleNamespace(value=active_route[0]),
                                   transition="stable", ready=active_route[0] != "none")
        def exposure():
            return SimpleNamespace(desired=SimpleNamespace(value="hidden" if hidden[0] else "exposed"),
                observed=SimpleNamespace(value="idle" if hidden[0] else "connected"),
                connected=not hidden[0], advertising=False, recovery_required=False, last_error=None)
        class Client:
            boot_id = "boot"
            session = "session"
            capabilities = ()
            def ping(self):
                if self.session is None:
                    raise SessionLost()
            def close(self):
                self.session = None
            def info(self): return {}
            def hid_route_status(self): return route()
            def ble_exposure_status(self): return exposure()
            def ble_pairing_status(self): return None
            def ble_bond_list(self): return SimpleNamespace(bonds=(), healthy=True)
            def hid_route_set(self, requested):
                self.ping()
                active_route[0] = requested.value
                self.session = None  # actual Client route-set retirement contract
                return route()
            def release_all(self):
                self.ping()
                self.assert_ble = active_route[0] == "ble"
                if not self.assert_ble: raise AssertionError("release lost BLE")
                return rehearsal.ReleaseResultForTest()  # patched below
            def ble_disable(self): hidden[0] = True
        runner = rehearsal.Rehearsal("unused", object(), lambda operation: None)
        def acquire(label):
            if active_route[0] == "ble" and label != "attempt":
                raise AssertionError("fresh hello after BLE route activation")
            labels.append(label)
            return Client()
        runner.acquire = acquire
        observer = mock.Mock()
        observer.collect.return_value = observation_complete()
        from dataclasses import dataclass
        @dataclass
        class Release:
            keyboard: str = "already_up"
            mouse: str = "already_up"
        with mock.patch.object(rehearsal, "ReleaseResultForTest", Release, create=True), \
             mock.patch.object(rehearsal, "validate_system_info", return_value={}), \
             mock.patch.object(rehearsal, "compare_firmware_identity", return_value=SimpleNamespace(match=True)), \
             mock.patch.object(rehearsal, "_connect_target"), \
             mock.patch.object(rehearsal, "_ble_ready", return_value=True), \
             mock.patch.object(rehearsal.ExactObservers, "discover", return_value=observer):
            runner.preflight("unused")
            original = runner.attempt_client
            result = runner.cleanup()
            self.assertTrue(result["same_attempt_session"])
            self.assertIsNone(runner.attempt_client)
            self.assertIsNone(original.session)
            self.assertEqual(labels, ["preflight", "secure_ready", "route_setup", "attempt", "post_retirement"])
            self.assertEqual(runner.mutations["release_all"], 1)
            runner.close()


if __name__ == "__main__":
    unittest.main()
