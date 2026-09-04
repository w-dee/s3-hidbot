#!/usr/bin/env python3
"""Hardware-free executable coverage for the qualification harness."""

from __future__ import annotations

import errno
import importlib.util
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import contextmanager, redirect_stderr
from pathlib import Path
from types import SimpleNamespace

from qualification_harness import (
    BondSnapshot,
    EvidenceDocument,
    EvdevIdentity,
    F24Checkpoint,
    FreshSessionManager,
    InputEvent,
    InputNode,
    QualificationError,
    RelXCheckpoint,
    artifact_preflight,
    bounded_poll,
    capture_btmon,
    classify_node_access,
    compare_artifact_identity,
    derive_source_identity,
    evaluate_outcome,
    inspect_bluez_paired,
    rediscover_input,
    safe_quiescent_cleanup,
    validate_ble_exposure,
    validate_delivery_checkpoint,
    validate_no_automatic_restore,
    validate_route_sequence,
    validate_usb_exposure,
)
from qualification_harness.artifact import parse_partition_geometry
from qualification_harness.core import PollTimeout, StepResult
from qualification_harness.input import EV_KEY, EV_REL, EV_SYN, KEY_F24, REL_X, SYN_REPORT


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        return self.value

    def sleep(self, duration: float) -> None:
        self.value += duration


class PollTests(unittest.TestCase):
    def test_success_is_bounded(self) -> None:
        clock = FakeClock()
        values = iter([0, 1, 2])
        result = bounded_poll(
            lambda: next(values), lambda value: value == 2,
            timeout=1, interval=0.1, label="ready", clock=clock, sleeper=clock.sleep,
        )
        self.assertEqual((result.value, result.attempts, result.elapsed_ms), (2, 3, 200))

    def test_timeout_retains_terminal_diagnostic(self) -> None:
        clock = FakeClock()
        with self.assertRaises(PollTimeout) as raised:
            bounded_poll(
                lambda: {"ready": False}, lambda value: value["ready"],
                timeout=0.2, interval=0.1, label="ready", clock=clock, sleeper=clock.sleep,
            )
        self.assertEqual(raised.exception.last, {"ready": False})
        self.assertEqual(raised.exception.attempts, 3)

    def test_backwards_monotonic_clock_is_rejected(self) -> None:
        ticks = iter([5.0, 4.0])
        with self.assertRaisesRegex(QualificationError, "backwards"):
            bounded_poll(
                lambda: False, bool, timeout=1, interval=0.1, label="clock",
                clock=lambda: next(ticks), sleeper=lambda _: None,
            )


class SessionTests(unittest.TestCase):
    def test_invalidation_requires_fresh_reacquisition(self) -> None:
        sequence = iter(("one", "two"))

        class Client:
            def __init__(self) -> None:
                self.session = next(sequence)

            def connect(self) -> SimpleNamespace:
                return SimpleNamespace(session=self.session, boot_id="boot")

            def close(self) -> None:
                pass

        manager = FreshSessionManager(lambda _timeout: Client())
        first = manager.acquire()
        manager.invalidate()
        with self.assertRaisesRegex(QualificationError, "stale"):
            manager.require_current(first)
        second = manager.acquire()
        self.assertNotEqual(first, second)

    def test_transport_retry_is_identical_and_limited_to_three(self) -> None:
        attempts: list[int] = []
        timeouts: list[float] = []

        class Client:
            def connect(self) -> None:
                attempts.append(1)
                raise OSError("transport")

            def close(self) -> None:
                pass

        with self.assertRaisesRegex(QualificationError, "3 attempts"):
            FreshSessionManager(
                lambda timeout: timeouts.append(timeout) or Client(),
                attempt_timeout=0.25,
            ).acquire()
        self.assertEqual(len(attempts), 3)
        self.assertEqual(timeouts, [0.25, 0.25, 0.25])


class SourceArtifactTests(unittest.TestCase):
    def test_source_identity_is_runtime_derived(self) -> None:
        def identity(revision: str) -> dict[str, object]:
            def run(args: list[str], _root: Path) -> str:
                if args[1:3] == ["rev-parse", "HEAD"]:
                    return revision + "\n"
                if args[1:3] == ["branch", "--show-current"]:
                    return "feature/test\n"
                return ""

            return derive_source_identity(Path("."), run=run)

        self.assertNotEqual(identity("1" * 40)["revision"], identity("2" * 40)["revision"])

    def test_partition_geometry_parser(self) -> None:
        import struct

        record = struct.pack("<2sBBLL16sL", b"\xaa\x50", 1, 2, 0x9000, 0x6000, b"nvs", 3)
        geometry = parse_partition_geometry(record + b"\xff" * 32)
        self.assertEqual(geometry[0]["label"], "nvs")
        self.assertTrue(geometry[0]["encrypted"])
        self.assertTrue(geometry[0]["readonly"])

    def test_artifact_preflight_uses_verified_bundle_and_omits_paths(self) -> None:
        import struct

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "artifact.tar.gz"
            archive.write_bytes(b"archive")
            table = root / "partition.bin"
            table.write_bytes(
                struct.pack("<2sBBLL16sL", b"\xaa\x50", 1, 2, 0x9000, 0x6000, b"nvs", 0)
                + b"\xff" * 32
            )
            hashes = {role: str(index) * 64 for index, role in enumerate(
                ("application_elf", "application_bin", "bootloader_bin", "partition_table_bin"), 1
            )}
            files = {f"{role}.file": {"role": role, "sha256": digest} for role, digest in hashes.items()}
            plan = SimpleNamespace(
                chip="esp32s3", before_reset="default_reset", after_reset="hard_reset",
                stub=True, flash_mode="dio", flash_size="4MB", flash_freq="80m",
                images=(
                    SimpleNamespace(role="bootloader_bin", offset=0, encrypted=False, path=root / "boot.bin"),
                    SimpleNamespace(role="partition_table_bin", offset=0x8000, encrypted=False, path=table),
                    SimpleNamespace(role="application_bin", offset=0x10000, encrypted=False, path=root / "app.bin"),
                ),
            )
            bundle = SimpleNamespace(
                manifest={
                    "artifact_manifest_version": 1, "project": "s3-hidbot",
                    "firmware": {"version": "0.1.0", "protocol_version": 1,
                                 "source_revision": "a" * 40, "target": "esp32s3",
                                 "build_profile": "freenove-fnk0085", "idf_version": "v5.5.4"},
                    "runtime_identity": {"app_elf_sha256": hashes["application_elf"]},
                    "files": files,
                },
                flash_plan=plan,
                verify_staged_payloads_unchanged=lambda: None,
            )

            @contextmanager
            def loader(_path: Path):
                yield bundle

            result = artifact_preflight(
                archive, source={"revision": "a" * 40}, loader=loader
            )
            self.assertEqual(result["verification"], "VALID")
            self.assertNotIn(str(root), json.dumps(result))

    def test_carry_forward_ignores_outer_archive_only(self) -> None:
        base = {
            "archive_sha256": "a" * 64, "payloads": {"license": "provenance-a"},
            "flash_payloads": {"application_bin": "b" * 64, "bootloader_bin": "e" * 64,
                               "partition_table_bin": "f" * 64},
            "firmware": {"source_revision": "c" * 40}, "runtime_elf_sha256": "d" * 64,
            "flash": {"chip": "esp32s3", "before": "default_reset", "after": "hard_reset",
                      "stub": True, "mode": "dio", "size": "4MB", "frequency": "80m",
                      "images": [
                          {"role": "bootloader_bin", "offset": 0, "sha256": "e" * 64, "encrypted": False},
                          {"role": "partition_table_bin", "offset": 0x8000, "sha256": "f" * 64, "encrypted": False},
                          {"role": "application_bin", "offset": 0x10000, "sha256": "b" * 64, "encrypted": False},
                      ]},
            "partitions": [{"label": "app"}],
        }
        other = json.loads(json.dumps(base))
        other["archive_sha256"] = "0" * 64
        other["payloads"]["license"] = "provenance-b"
        comparison = compare_artifact_identity(base, other)
        self.assertFalse(comparison["archive_byte_identity"])
        self.assertTrue(comparison["physical_qualification_carry_forward"])
        other["flash_payloads"]["application_bin"] = "different"
        self.assertFalse(compare_artifact_identity(base, other)["physical_qualification_carry_forward"])

    def test_incomplete_artifact_identity_fails_closed(self) -> None:
        self.assertFalse(
            compare_artifact_identity({}, {})["physical_qualification_carry_forward"]
        )
        self.assertFalse(
            compare_artifact_identity(
                {"firmware": "malformed", "flash": "malformed"},
                {"firmware": None, "flash": None},
            )["physical_qualification_carry_forward"]
        )


def bond_value(ids: list[str], capacity: int = 3) -> dict[str, object]:
    return {
        "available": capacity - len(ids), "capacity": capacity, "count": len(ids),
        "healthy": True, "bonds": [{"bond_id": item} for item in ids],
    }


class InvariantTests(unittest.TestCase):
    def test_bond_exact_comparison(self) -> None:
        ids = ["a" * 32, "b" * 32]
        self.assertTrue(BondSnapshot.from_value(bond_value(ids)).compare(
            BondSnapshot.from_value(bond_value(list(reversed(ids))))
        )["exact"])

    def test_bond_eviction_and_resurrection_are_reported(self) -> None:
        before = BondSnapshot.from_value(bond_value(["a" * 32, "b" * 32]))
        after = BondSnapshot.from_value(bond_value(["a" * 32, "c" * 32]))
        result = before.compare(after)
        self.assertTrue(result["unexpected_eviction"])
        self.assertTrue(result["unexpected_resurrection"])

    def test_route_switch_requires_stable_none(self) -> None:
        usb = {"desired": "usb", "active": "usb", "generation": 1, "transition": "stable", "ready": True}
        none = {"desired": "none", "active": "none", "generation": 2, "transition": "stable", "ready": False}
        ble = {"desired": "ble", "active": "ble", "generation": 3, "transition": "stable", "ready": True}
        self.assertEqual(len(validate_route_sequence([usb, none, ble])), 3)
        with self.assertRaisesRegex(QualificationError, "stable none"):
            validate_route_sequence([usb, ble])

    def test_route_does_not_restore_after_retirement(self) -> None:
        usb = {"desired": "usb", "active": "usb", "generation": 1, "transition": "stable", "ready": True}
        none = {"desired": "none", "active": "none", "generation": 2, "transition": "stable", "ready": False}
        self.assertEqual(len(validate_no_automatic_restore([usb, none, none])), 3)
        with self.assertRaisesRegex(QualificationError, "automatically"):
            validate_no_automatic_restore([usb, none, usb])

    def test_delivery_checkpoint_rejects_dual_delivery(self) -> None:
        self.assertTrue(validate_delivery_checkpoint(
            expected_route="usb", usb_deliveries=1, ble_deliveries=0
        )["single_delivery"])
        with self.assertRaisesRegex(QualificationError, "dual-routed"):
            validate_delivery_checkpoint(
                expected_route="usb", usb_deliveries=1, ble_deliveries=1
            )

    def test_ble_state_coherence(self) -> None:
        good = {"desired": "exposed", "observed": "advertising", "generation": 1,
                "stack_ready": True, "advertising": True, "connected": False,
                "recovery_required": False, "last_error": None}
        self.assertEqual(validate_ble_exposure(good)["observed"], "advertising")
        bad = dict(good, connected=True)
        with self.assertRaises(QualificationError):
            validate_ble_exposure(bad)
        with self.assertRaises(QualificationError):
            validate_ble_exposure(dict(good, generation=True))

    def test_usb_state_coherence(self) -> None:
        good = {
            "desired": "exposed", "observed": "mounted", "generation": 1,
            "mounted": True, "suspended": False, "keyboard_ready": True,
            "mouse_ready": True, "safety_pending": False,
            "host_release_uncertain": False, "recovery_required": False,
            "last_error": None,
        }
        self.assertEqual(validate_usb_exposure(good)["observed"], "mounted")
        self.assertEqual(
            validate_usb_exposure(dict(good, mouse_ready=False))["observed"],
            "mounted",
        )
        with self.assertRaises(QualificationError):
            validate_usb_exposure(dict(good, mounted=False))


class InputTests(unittest.TestCase):
    identity = EvdevIdentity("s3-hidbot", 5, 1, 2, 3, "keyboard")

    def test_expected_and_unexpected_retirement(self) -> None:
        error = OSError(errno.ENODEV, "gone")
        self.assertEqual(classify_node_access(error, retirement_expected=True), "expected_retirement")
        with self.assertRaises(QualificationError):
            classify_node_access(error, retirement_expected=False)

    def test_rediscovery_returns_new_event_node(self) -> None:
        clock = FakeClock()
        calls = iter(([], [InputNode("event27", self.identity)]))
        result = rediscover_input(
            lambda: next(calls), self.identity, previous=InputNode("event13", self.identity),
            timeout=1, interval=0.1, clock=clock, sleeper=clock.sleep,
        )
        self.assertEqual(result["event_name"], "event27")
        self.assertTrue(result["path_changed"])

    def test_ambiguous_rediscovery_fails(self) -> None:
        nodes = [InputNode("event1", self.identity), InputNode("event2", self.identity)]
        with self.assertRaisesRegex(QualificationError, "ambiguous"):
            rediscover_input(
                lambda: nodes, self.identity, timeout=1, interval=0.1,
                clock=lambda: 0, sleeper=lambda _: None,
            )

    def test_f24_down_repeat_up_is_accepted(self) -> None:
        checkpoint = F24Checkpoint()
        for value in (1, 2, 2, 0):
            checkpoint.observe(InputEvent(EV_KEY, KEY_F24, value))
        self.assertEqual(checkpoint.finish()["repeat_count"], 2)

    def test_f24_repeat_before_down_is_rejected(self) -> None:
        with self.assertRaises(QualificationError):
            F24Checkpoint().observe(InputEvent(EV_KEY, KEY_F24, 2))

    def test_f24_repeat_after_up_is_rejected(self) -> None:
        checkpoint = F24Checkpoint()
        checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 1))
        checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 0))
        with self.assertRaises(QualificationError):
            checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 2))

    def test_f24_fresh_replay_after_release_is_rejected(self) -> None:
        checkpoint = F24Checkpoint()
        checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 1))
        checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 0))
        with self.assertRaisesRegex(QualificationError, "replay"):
            checkpoint.observe(InputEvent(EV_KEY, KEY_F24, 1))

    def test_rel_x_success(self) -> None:
        checkpoint = RelXCheckpoint()
        checkpoint.observe(InputEvent(EV_REL, REL_X, 1))
        checkpoint.observe(InputEvent(EV_SYN, SYN_REPORT, 0))
        self.assertTrue(checkpoint.finish()["syn_report"])

    def test_rel_x_failure(self) -> None:
        checkpoint = RelXCheckpoint()
        with self.assertRaises(QualificationError):
            checkpoint.observe(InputEvent(EV_REL, REL_X, -1))
        with self.assertRaises(QualificationError):
            RelXCheckpoint().finish()


class OutcomeCleanupEvidenceTests(unittest.TestCase):
    def test_main_failure_cleanup_success_preserves_main(self) -> None:
        result = evaluate_outcome(StepResult("fail", "functional"), StepResult("pass"))
        self.assertEqual(result["overall"], "fail")
        self.assertEqual(result["failed_parts"], ["main"])

    def test_main_success_cleanup_failure_is_not_pass(self) -> None:
        result = evaluate_outcome(StepResult("pass"), StepResult("fail", "cleanup"))
        self.assertEqual(result["overall"], "fail")

    def test_both_failures_are_preserved(self) -> None:
        result = evaluate_outcome(StepResult("fail", "main"), StepResult("fail", "cleanup"))
        self.assertEqual(result["failed_parts"], ["main", "cleanup"])

    def test_safe_cleanup_uses_fresh_clients_and_one_release(self) -> None:
        actions: list[str] = []

        class Client:
            def release_all(self) -> None: actions.append("release")
            def hid_route_set(self, route: str) -> None: actions.append("route:" + route)
            def usb_detach(self) -> None: actions.append("usb")
            def ble_disable(self) -> None: actions.append("ble")
            def hid_route_status(self) -> dict[str, object]:
                return {"desired": "none", "active": "none", "generation": 1,
                        "transition": "stable", "ready": False}
            def usb_exposure_status(self) -> dict[str, object]:
                return {"desired": "hidden", "observed": "driver_not_installed",
                        "generation": 1, "mounted": False, "suspended": False,
                        "keyboard_ready": False, "mouse_ready": False,
                        "safety_pending": False, "host_release_uncertain": False,
                        "recovery_required": False, "last_error": None}
            def ble_exposure_status(self) -> dict[str, object]:
                return {"desired": "hidden", "observed": "idle", "generation": 1,
                        "stack_ready": True, "advertising": False, "connected": False,
                        "recovery_required": False, "last_error": None}
            def close(self) -> None: actions.append("close")

        acquired: list[Client] = []
        def acquire() -> Client:
            client = Client()
            acquired.append(client)
            return client

        result = safe_quiescent_cleanup(acquire)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(actions.count("release"), 1)
        self.assertEqual(len(acquired), 5)

    def test_evidence_serialization_is_stable_and_machine_readable(self) -> None:
        document = EvidenceDocument(source={"revision": "a" * 40, "dirty": False}, started_at="2026-01-01T00:00:00Z")
        one = document.serialize(duration_ms=1, main=StepResult("pass"), cleanup=StepResult("not_required"))
        two = document.serialize(duration_ms=1, main=StepResult("pass"), cleanup=StepResult("not_required"))
        self.assertEqual(one, two)
        self.assertEqual(json.loads(one)["schema"], "s3-hidbot-qualification-evidence")

    def test_evidence_rejects_serial_paths_and_envrc_content(self) -> None:
        for secret in (
            "/dev/" + "serial/" + "by-id/test-device",
            ".envrc secret content",
            "AA:BB:CC:DD:EE:FF",
            "passkey=123456",
        ):
            document = EvidenceDocument(source={"diagnostic": secret}, started_at="now")
            with self.assertRaises(QualificationError):
                document.serialize(duration_ms=0, main=StepResult("fail"), cleanup=StepResult("pass"))
        document = EvidenceDocument(
            source={"pairing_passkey": "123456"}, started_at="now"
        )
        with self.assertRaises(QualificationError):
            document.serialize(
                duration_ms=0, main=StepResult("fail"), cleanup=StepResult("pass")
            )


class BtmonBluezRunnerTests(unittest.TestCase):
    def test_btmon_timeout_cleanup_uses_sigint(self) -> None:
        class Process:
            def __init__(self, output) -> None:
                self.output = output
                self.signals: list[int] = []
                self.exited = False
            def poll(self): return 0 if self.exited else None
            def send_signal(self, value: int) -> None:
                self.signals.append(value)
                self.output.write(b"Service Changed\nReport Map\n")
                self.output.flush()
            def wait(self, timeout: int) -> int:
                self.exited = True
                return 0
            def terminate(self) -> None: raise AssertionError("unexpected terminate")
            def kill(self) -> None: raise AssertionError("unexpected kill")

        holder: list[Process] = []
        def factory(output):
            process = Process(output)
            holder.append(process)
            return process

        with tempfile.TemporaryDirectory() as temporary:
            summary = capture_btmon(
                Path(temporary) / "capture.txt", duration=0.1,
                process_factory=factory, sleeper=lambda _: None,
            )
        self.assertEqual(holder[0].signals, [2])
        self.assertEqual(summary["service_changed_indications"], 1)
        self.assertEqual(summary["report_map_reads"], 1)

    def test_btmon_escalates_to_terminate_after_bounded_wait(self) -> None:
        class Process:
            def __init__(self) -> None:
                self.terminated = False
                self.waits = 0
            def poll(self): return 0 if self.terminated else None
            def send_signal(self, _value: int) -> None: pass
            def wait(self, timeout: int) -> int:
                self.waits += 1
                if self.waits == 1:
                    raise subprocess.TimeoutExpired("btmon", timeout)
                self.terminated = True
                return -15
            def terminate(self) -> None: self.terminated = True
            def kill(self) -> None: self.terminated = True

        process = Process()
        with tempfile.TemporaryDirectory() as temporary:
            summary = capture_btmon(
                Path(temporary) / "capture.txt", duration=0.1,
                process_factory=lambda _output: process, sleeper=lambda _: None,
            )
        self.assertTrue(summary["forced_stop"])
        self.assertEqual(summary["exit_code"], -15)

    def test_bluez_inspection_is_fixed_and_address_free(self) -> None:
        commands: list[list[str]] = []
        def run(command: list[str]) -> str:
            commands.append(command)
            return "Device AA:BB:CC:DD:EE:FF s3-hidbot\nDevice 00:11:22:33:44:55 headset\n"
        result = inspect_bluez_paired(run)
        self.assertEqual(commands, [["bluetoothctl", "devices", "Paired"]])
        self.assertEqual(result["match_count"], 1)
        self.assertNotIn("AA:BB", json.dumps(result))

    def test_runner_has_no_bluez_delete_surface(self) -> None:
        runner_path = Path(__file__).with_name("qualification_runner.py")
        spec = importlib.util.spec_from_file_location("qualification_runner_test", runner_path)
        assert spec and spec.loader
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        parser = runner.build_parser()
        parsed = parser.parse_args(["preflight", "--evidence", "evidence.json"])
        self.assertEqual(parsed.command, "preflight")
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args(["remove-bluez"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
