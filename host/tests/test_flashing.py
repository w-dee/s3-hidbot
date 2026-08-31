from __future__ import annotations

import os
import tempfile
import unittest
from importlib.metadata import PackageNotFoundError
from pathlib import Path
from unittest import mock

from hidbot.artifact import ArtifactError
from hidbot.flashing import (
    MAX_DIAGNOSTIC_TAIL_BYTES,
    FlashDependencyError,
    FlashExecutionError,
    _ProcessOutcome,
    execute_flash,
    require_esptool,
)
from hidbot.provisioning import stage_and_verify_firmware_bundle

from test_provisioning import _make_bundle


class FlashingTests(unittest.TestCase):
    def bundle(self):
        temporary = tempfile.TemporaryDirectory(prefix="s3-hidbot-flash-test-")
        self.addCleanup(temporary.cleanup)
        source = _make_bundle(Path(temporary.name) / "source")
        staged = stage_and_verify_firmware_bundle(source)
        bundle = staged.__enter__()
        self.addCleanup(staged.__exit__, None, None, None)
        return bundle

    @staticmethod
    def version(_: str) -> str:
        return "4.12.0"

    def test_success_uses_exact_planner_argv_and_private_environment(self) -> None:
        bundle = self.bundle()
        calls: list[tuple[tuple[str, ...], dict[str, object]]] = []

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            calls.append((argv, kwargs))
            cwd = kwargs["cwd"]
            self.assertIsInstance(cwd, Path)
            self.assertTrue(cwd.is_dir())
            environment = kwargs["env"]
            self.assertIsInstance(environment, dict)
            assert isinstance(environment, dict)
            self.assertNotIn("ESPTOOL_ENV_FPGA", environment)
            self.assertTrue(Path(environment["ESPTOOL_CFGFILE"]).is_file())
            self.assertEqual(
                Path(environment["ESPTOOL_CFGFILE"]).read_text(encoding="ascii"),
                "[esptool]\n",
            )
            self.assertFalse(kwargs["shell"])
            self.assertEqual(kwargs["timeout"], 300.0)
            self.assertTrue(kwargs["capture_output"])
            return _ProcessOutcome(0)

        with mock.patch.dict(
            os.environ,
            {"ESPTOOL_ENV_FPGA": "1", "ESPTOOL_BAUD": "9600", "FLASH_TEST_KEEP": "yes"},
            clear=False,
        ):
            result = execute_flash(
                bundle,
                "port with spaces;and$metacharacters",
                json_mode=True,
                process_runner=runner,
                version_provider=self.version,
            )
        self.assertEqual(result.attempts, 1)
        self.assertEqual(result.chip, "esp32s3")
        self.assertEqual(result.image_count, 3)
        self.assertEqual(len(calls), 1)
        argv, kwargs = calls[0]
        self.assertEqual(argv[:3], (os.sys.executable, "-m", "esptool"))
        self.assertEqual(argv[argv.index("--port") + 1], "port with spaces;and$metacharacters")
        self.assertNotIn("--baud", argv)
        self.assertNotIn("--no-stub", argv)
        self.assertNotIn("erase_flash", argv)
        paths = [str(image.path) for image in bundle.flash_plan.images]
        self.assertTrue(all(path in argv for path in paths))
        self.assertFalse(Path(kwargs["cwd"]).exists())

    def test_nonzero_then_success_retries_identical_operation(self) -> None:
        bundle = self.bundle()
        calls: list[tuple[str, ...]] = []
        outcomes = iter((_ProcessOutcome(1), _ProcessOutcome(0)))

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            del kwargs
            calls.append(argv)
            return next(outcomes)

        result = execute_flash(
            bundle, "port", process_runner=runner, version_provider=self.version
        )
        self.assertEqual(result.attempts, 2)
        self.assertEqual(calls[0], calls[1])

    def test_three_nonzero_attempts_return_programming_failure(self) -> None:
        bundle = self.bundle()
        calls: list[tuple[str, ...]] = []

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            del kwargs
            calls.append(argv)
            return _ProcessOutcome(1, diagnostic_tail=b"x" * (MAX_DIAGNOSTIC_TAIL_BYTES + 20))

        with self.assertRaises(FlashExecutionError) as raised:
            execute_flash(
                bundle, "port", process_runner=runner, version_provider=self.version
            )
        self.assertEqual(raised.exception.attempts, 3)
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[0], calls[1])
        self.assertEqual(calls[1], calls[2])
        self.assertEqual(len(raised.exception.diagnostic_tail), MAX_DIAGNOSTIC_TAIL_BYTES)

    def test_timeout_then_success_retries_without_mutation(self) -> None:
        bundle = self.bundle()
        calls: list[tuple[str, ...]] = []
        outcomes = iter((_ProcessOutcome(-15, timed_out=True), _ProcessOutcome(0)))

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            del kwargs
            calls.append(argv)
            return next(outcomes)

        result = execute_flash(
            bundle, "port", process_runner=runner, version_provider=self.version
        )
        self.assertEqual(result.attempts, 2)
        self.assertEqual(calls[0], calls[1])

    def test_timeout_exhaustion_is_exit_eight_error(self) -> None:
        bundle = self.bundle()
        calls = 0

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            nonlocal calls
            del argv, kwargs
            calls += 1
            return _ProcessOutcome(-15, timed_out=True)

        with self.assertRaises(FlashExecutionError) as raised:
            execute_flash(
                bundle, "port", process_runner=runner, version_provider=self.version
            )
        self.assertTrue(raised.exception.timed_out)
        self.assertEqual(calls, 3)

    def test_staged_mutation_stops_before_retry(self) -> None:
        bundle = self.bundle()
        calls = 0

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            nonlocal calls
            del argv, kwargs
            calls += 1
            (bundle.staged_root / "application.bin").write_bytes(b"mutated")
            return _ProcessOutcome(1)

        with self.assertRaises(ArtifactError):
            execute_flash(
                bundle, "port", process_runner=runner, version_provider=self.version
            )
        self.assertEqual(calls, 1)

    def test_keyboard_interrupt_does_not_retry_and_cleans_execution_directory(self) -> None:
        bundle = self.bundle()
        seen_cwd: list[Path] = []

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            del argv
            seen_cwd.append(kwargs["cwd"])
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            execute_flash(
                bundle, "port", process_runner=runner, version_provider=self.version
            )
        self.assertEqual(len(seen_cwd), 1)
        self.assertFalse(seen_cwd[0].exists())

    def test_unavailable_unreadable_or_incompatible_esptool_never_runs_process(self) -> None:
        bundle = self.bundle()
        for version_provider in (
            lambda _: (_ for _ in ()).throw(PackageNotFoundError("esptool")),
            lambda _: (_ for _ in ()).throw(OSError("metadata unreadable")),
            lambda _: None,
            lambda _: "4.11.0",
            lambda _: "5.0.0",
        ):
            calls = 0

            def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
                nonlocal calls
                del argv, kwargs
                calls += 1
                return _ProcessOutcome(0)

            with self.assertRaises(FlashDependencyError):
                execute_flash(
                    bundle,
                    "port",
                    process_runner=runner,
                    version_provider=version_provider,
                )
            self.assertEqual(calls, 0)

    def test_supported_version_range_is_strict(self) -> None:
        for value in ("4.12", "4.12.0", "4.99.7"):
            with self.subTest(value=value):
                self.assertEqual(require_esptool(lambda _: value), value)
        for value in ("4.11.9", "4.12.0rc1", "5.0.0", "not-a-version"):
            with self.subTest(value=value):
                with self.assertRaises(FlashDependencyError):
                    require_esptool(lambda _: value)

    def test_process_start_failure_is_a_non_retrying_programming_error(self) -> None:
        bundle = self.bundle()
        calls = 0

        def runner(argv: tuple[str, ...], **kwargs: object) -> _ProcessOutcome:
            nonlocal calls
            del argv, kwargs
            calls += 1
            raise OSError("process unavailable")

        with self.assertRaises(FlashExecutionError) as raised:
            execute_flash(bundle, "port", process_runner=runner, version_provider=self.version)
        self.assertEqual(raised.exception.attempts, 1)
        self.assertEqual(calls, 1)
