from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from collections import deque
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable
from unittest.mock import patch

from hidbot.artifact import ArtifactError, create_deterministic_tar_gz, sha256_file, write_deterministic_json
from hidbot.cli import main
from hidbot.errors import FlashExecutionError, TransportError
from hidbot.flashing import FlashExecutionResult
from hidbot.firmware_verification import (
    ArtifactFirmwareIdentity,
    FirmwareVerificationClassification,
    FirmwareIdentityMismatch,
    FirmwareVerificationResult,
    IdentityUnavailableReason,
)
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC
from hidbot.provisioning_workflow import (
    ProvisioningWorkflowResult,
    VerificationPhaseClassification,
    VerificationPhaseResult,
)
from hidbot.protocol import FirmwareIdentity, SystemInfo


TOKEN = "0123456789abcdef0123456789abcdef"
NONCE = "fedcba9876543210fedcba9876543210"


def response(
    request_id: int,
    session: str | None,
    *,
    result: object = None,
    error: dict[str, str] | None = None,
) -> bytes:
    value: dict[str, object] = {
        "type": "response",
        "v": 1,
        "id": request_id,
        "session": session,
        "ok": error is None,
    }
    if error is None:
        value["result"] = {} if result is None else result
    else:
        value["error"] = error
    return FRAME_PREFIX + json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"


class FakeTransport:
    def __init__(self, calls: list[tuple[object, ...]], **kwargs: object) -> None:
        self.calls = calls
        self.calls.append(("construct", kwargs))
        self.chunks: deque[bytes] = deque()
        self.closed = False
        self.hello_capabilities = [
            "protocol.hello-v1",
            "system.ping-v1",
            "system.info-v1",
            "usb.status-v1",
            "usb.exposure-control-v1",
            "hid.lease-v1",
            "hid.release-all-v1",
            "hid.keyboard-report-v1",
            "hid.mouse-report-v1",
            "hid.output-route-v1",
        ]
        self.info_result: object = {"project": "s3-hidbot"}

    def open(self) -> None:
        self.calls.append(("open",))

    def close(self) -> None:
        self.calls.append(("close",))
        self.closed = True

    def write(self, data: bytes) -> None:
        self.calls.append(("write", data))
        if data == TRANSPORT_SYNC:
            return
        value = json.loads(data[len(FRAME_PREFIX) : -1])
        if value["cmd"] == "protocol.hello":
            self.chunks.append(
                response(
                    value["id"],
                    TOKEN,
                    result={
                        "project": "s3-hidbot",
                        "protocol_version": 1,
                        "client_nonce": value["params"]["client_nonce"],
                        "boot_id": TOKEN,
                        "session": TOKEN,
                        "lease_ms": 5000,
                        "capabilities": self.hello_capabilities,
                    },
                )
            )
        elif value["cmd"] == "system.ping":
            self.chunks.append(response(value["id"], TOKEN, result={"pong": True}))
        elif value["cmd"] == "system.info":
            self.chunks.append(response(value["id"], TOKEN, result=self.info_result))
        elif value["cmd"] == "hid.release_all":
            self.chunks.append(
                response(
                    value["id"],
                    TOKEN,
                    result={"keyboard": "already_up", "mouse": "submitted"},
                )
            )
        elif value["cmd"] in {"usb.exposure.status", "usb.attach", "usb.detach"}:
            if value["cmd"] == "usb.attach":
                result = {
                    "desired": "exposed",
                    "observed": "attaching",
                    "generation": 1,
                    "mounted": False,
                    "suspended": False,
                    "keyboard_ready": False,
                    "mouse_ready": False,
                    "safety_pending": False,
                    "host_release_uncertain": False,
                    "recovery_required": False,
                    "last_error": None,
                }
            elif value["cmd"] == "usb.detach":
                result = {
                    "desired": "hidden",
                    "observed": "detaching",
                    "generation": 1,
                    "mounted": True,
                    "suspended": False,
                    "keyboard_ready": True,
                    "mouse_ready": True,
                    "safety_pending": True,
                    "host_release_uncertain": False,
                    "recovery_required": False,
                    "last_error": None,
                }
            else:
                result = {
                    "desired": "hidden",
                    "observed": "driver_not_installed",
                    "generation": 0,
                    "mounted": False,
                    "suspended": False,
                    "keyboard_ready": False,
                    "mouse_ready": False,
                    "safety_pending": False,
                    "host_release_uncertain": False,
                    "recovery_required": False,
                    "last_error": None,
                }
            self.chunks.append(response(value["id"], TOKEN, result=result))
        elif value["cmd"] in {"hid.route.status", "hid.route.set"}:
            route = value["params"].get("route", "none")
            self.chunks.append(
                response(
                    value["id"],
                    TOKEN,
                    result={
                        "desired": route,
                        "active": route,
                        "generation": 1 if route == "usb" else 0,
                        "transition": "stable",
                        "ready": route == "usb",
                    },
                )
            )
        elif value["cmd"] in {"hid.keyboard.report", "hid.mouse.report"}:
            params = value["params"]
            if value["cmd"] == "hid.keyboard.report":
                already_set = params["modifiers"] == 0 and params["keys"] == []
            else:
                already_set = (
                    params["buttons"] == 0
                    and params["x"] == 0
                    and params["y"] == 0
                    and params["wheel"] == 0
                    and params["pan"] == 0
                )
            state = "already_set" if already_set else "submitted"
            self.chunks.append(response(value["id"], TOKEN, result={"state": state}))
        else:
            self.chunks.append(response(value["id"], TOKEN, result={"mounted": True}))

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del max_bytes, timeout
        return self.chunks.popleft() if self.chunks else b""


class CliTests(unittest.TestCase):
    def run_cli(
        self,
        argv: list[str],
        env: dict[str, str] | None = None,
        configure_transport: Callable[[FakeTransport], None] | None = None,
    ) -> tuple[int, str, str, list[tuple[object, ...]]]:
        calls: list[tuple[object, ...]] = []
        transport = FakeTransport(calls)
        if configure_transport is not None:
            configure_transport(transport)

        def factory(*args: object, **kwargs: object) -> FakeTransport:
            calls.append(("factory", args, kwargs))
            return transport

        output = io.StringIO()
        errors = io.StringIO()
        code = main(
            argv,
            environ={} if env is None else env,
            transport_factory=factory,
            output=output,
            error_output=errors,
        )
        return code, output.getvalue(), errors.getvalue(), calls

    def run_artifact_only(
        self, argv: list[str], env: dict[str, str] | None = None
    ) -> tuple[int, str, str, list[tuple[object, ...]]]:
        """Run an artifact-only command with a transport factory that must not run."""

        calls: list[tuple[object, ...]] = []

        def factory(*args: object, **kwargs: object) -> object:
            calls.append(("factory", args, kwargs))
            raise AssertionError("artifact-only command constructed a transport")

        output = io.StringIO()
        errors = io.StringIO()
        code = main(
            argv,
            environ={} if env is None else env,
            transport_factory=factory,
            output=output,
            error_output=errors,
        )
        return code, output.getvalue(), errors.getvalue(), calls

    def run_parser_error(
        self, argv: list[str], env: dict[str, str] | None = None
    ) -> tuple[int, str, list[tuple[object, ...]]]:
        calls: list[tuple[object, ...]] = []

        def factory(*args: object, **kwargs: object) -> object:
            calls.append(("factory", args, kwargs))
            raise AssertionError("transport must not be constructed")

        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            try:
                code = main(
                    argv,
                    environ={} if env is None else env,
                    transport_factory=factory,
                    output=io.StringIO(),
                    error_output=errors,
                )
            except SystemExit as exc:
                code = int(exc.code)
        return code, errors.getvalue(), calls

    def run_cli_with_transport(
        self,
        argv: list[str],
        transport: FakeTransport,
        env: dict[str, str] | None = None,
    ) -> tuple[int, str, str, list[tuple[object, ...]]]:
        calls = transport.calls

        def factory(*args: object, **kwargs: object) -> FakeTransport:
            calls.append(("factory", args, kwargs))
            return transport

        output = io.StringIO()
        errors = io.StringIO()
        code = main(
            argv,
            environ={} if env is None else env,
            transport_factory=factory,
            output=output,
            error_output=errors,
        )
        return code, output.getvalue(), errors.getvalue(), calls

    def run_flash_cli(
        self,
        argv: list[str],
        *,
        env: dict[str, str] | None = None,
        executor: Callable[..., FlashExecutionResult] | None = None,
        workflow_runner: Callable[..., ProvisioningWorkflowResult] | None = None,
    ) -> tuple[int, str, str, list[tuple[object, ...]]]:
        calls: list[tuple[object, ...]] = []
        identity = ArtifactFirmwareIdentity(
            project="s3-hidbot",
            target="esp32s3",
            protocol_version=1,
            version="0.1.0-dev",
            source_revision="a" * 40,
            app_elf_sha256="b" * 64,
            build_profile="freenove-fnk0085",
            idf_version="v5.5.4",
        )
        bundle = SimpleNamespace(artifact_identity=identity)

        def transport_factory(*args: object, **kwargs: object) -> object:
            calls.append(("transport", args, kwargs))
            raise AssertionError("flash-firmware must not construct a serial transport")

        if executor is None:
            def executor(bundle_value: object, port: str, **kwargs: object) -> FlashExecutionResult:
                calls.append(("flash", bundle_value, port, kwargs))
                return FlashExecutionResult(attempts=1, chip="esp32s3", image_count=3)

        if workflow_runner is None:
            def workflow_runner(bundle_value: object, port: str, **kwargs: object) -> ProvisioningWorkflowResult:
                calls.append(("workflow", bundle_value, port, kwargs))
                flash = kwargs["flash_executor"](bundle_value, port, json_mode=kwargs["json_mode"], on_retry=kwargs["on_retry"])
                assert isinstance(flash, FlashExecutionResult)
                device = SystemInfo(
                    project="s3-hidbot",
                    target="esp32s3",
                    idf_version="v5.5.4",
                    protocol_version=1,
                    firmware=FirmwareIdentity(
                        version="0.1.0-dev",
                        source_revision="a" * 40,
                        app_elf_sha256="b" * 64,
                        build_profile="freenove-fnk0085",
                    ),
                )
                comparison = FirmwareVerificationResult(
                    match=True,
                    classification=FirmwareVerificationClassification.MATCH,
                    mismatches=(),
                    unavailable_reason=None,
                    artifact=identity,
                    device=device,
                )
                return ProvisioningWorkflowResult(
                    flash=flash,
                    verification=VerificationPhaseResult(
                        classification=VerificationPhaseClassification.MATCH,
                        reconnect_attempts=1,
                        boot_id=TOKEN,
                        firmware_verification=comparison,
                    ),
                )

        output = io.StringIO()
        errors = io.StringIO()
        with patch("hidbot.cli.stage_and_verify_firmware_bundle", return_value=contextlib.nullcontext(bundle)):
            code = main(
                argv,
                environ={} if env is None else env,
                transport_factory=transport_factory,
                flash_executor=executor,
                provisioning_workflow_runner=workflow_runner,
                output=output,
                error_output=errors,
            )
        return code, output.getvalue(), errors.getvalue(), calls

    @staticmethod
    def flash_workflow_result(
        identity: ArtifactFirmwareIdentity,
        classification: VerificationPhaseClassification,
    ) -> ProvisioningWorkflowResult:
        device = SystemInfo(
            project="s3-hidbot",
            target="esp32c6" if classification is VerificationPhaseClassification.MISMATCH else "esp32s3",
            idf_version="v5.5.4",
            protocol_version=1,
            firmware=FirmwareIdentity(
                version="0.1.0-dev",
                source_revision="a" * 40,
                app_elf_sha256="b" * 64,
                build_profile="freenove-fnk0085",
            ),
        )
        comparison: FirmwareVerificationResult | None = None
        if classification is VerificationPhaseClassification.MISMATCH:
            comparison = FirmwareVerificationResult(
                match=False,
                classification=FirmwareVerificationClassification.MISMATCH,
                mismatches=(FirmwareIdentityMismatch.TARGET_MISMATCH,),
                unavailable_reason=None,
                artifact=identity,
                device=device,
            )
        elif classification is VerificationPhaseClassification.IDENTITY_UNAVAILABLE:
            comparison = FirmwareVerificationResult(
                match=False,
                classification=FirmwareVerificationClassification.IDENTITY_UNAVAILABLE,
                mismatches=(),
                unavailable_reason=IdentityUnavailableReason.SOURCE_REVISION_UNAVAILABLE,
                artifact=identity,
                device=device,
            )
        return ProvisioningWorkflowResult(
            flash=FlashExecutionResult(attempts=1, chip="esp32s3", image_count=3),
            verification=VerificationPhaseResult(
                classification=classification,
                reconnect_attempts=2,
                boot_id=TOKEN if comparison is not None else None,
                firmware_verification=comparison,
            ),
        )

    @staticmethod
    def wire_commands(calls: list[tuple[object, ...]]) -> list[str]:
        return [
            json.loads(call[1][len(FRAME_PREFIX) : -1])["cmd"]
            for call in calls
            if call[0] == "write" and call[1] != TRANSPORT_SYNC
        ]

    def test_port_baud_overrides_and_json_hello(self) -> None:
        code, output, errors, calls = self.run_cli(
            ["--port", "dummy-port", "--baud", "230400", "--json", "hello"],
            {"S3_HIDBOT_SERIAL": "env-port", "S3_HIDBOT_BAUD": "9600"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        value = json.loads(output)
        self.assertEqual(value["session"], TOKEN)
        factory_call = next(call for call in calls if call[0] == "factory")
        self.assertEqual(factory_call[1], ("dummy-port", 230400))
        kwargs = factory_call[2]
        assert isinstance(kwargs, dict)
        self.assertEqual(kwargs["read_timeout"], 0.05)
        self.assertEqual(kwargs["write_timeout"], 1.0)

    def test_env_fallback_and_commands(self) -> None:
        for command in ("ping", "info", "usb-status", "release-all", "self-test"):
            code, output, errors, _ = self.run_cli(
                [command],
                {"S3_HIDBOT_SERIAL": "env-port", "S3_HIDBOT_BAUD": "115200"},
            )
            self.assertEqual(code, 0, command)
            self.assertEqual(errors, "", command)
            self.assertTrue(output.strip(), command)

        code, _, errors, calls = self.run_cli(["hello"], {"S3_HIDBOT_SERIAL": "env-port"})
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        factory_call = next(call for call in calls if call[0] == "factory")
        self.assertEqual(factory_call[1], ("env-port", 115200))

    def test_release_all_success_and_protocol_sequence(self) -> None:
        code, output, errors, calls = self.run_cli(
            ["--port", "dummy-port", "release-all"],
            {"S3_HIDBOT_SERIAL": "env-port"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(
            json.loads(output), {"keyboard": "already_up", "mouse": "submitted"}
        )
        commands = [
            json.loads(call[1][len(FRAME_PREFIX) : -1])["cmd"]
            for call in calls
            if call[0] == "write" and call[1] != TRANSPORT_SYNC
        ]
        self.assertEqual(commands, ["protocol.hello", "hid.release_all"])

    def test_release_all_json_uses_compact_result_policy(self) -> None:
        code, output, errors, _ = self.run_cli(
            ["--port", "dummy-port", "--json", "release-all"],
            {"S3_HIDBOT_SERIAL": "env-port"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(output, '{"keyboard":"already_up","mouse":"submitted"}\n')

    def test_usb_exposure_operator_commands_are_explicit(self) -> None:
        expected = {
            "usb-exposure-status": ("usb.exposure.status", "hidden", "driver_not_installed"),
            "usb-attach": ("usb.attach", "exposed", "attaching"),
            "usb-detach": ("usb.detach", "hidden", "detaching"),
        }
        for command, (wire_command, desired, observed) in expected.items():
            code, output, errors, calls = self.run_cli(
                ["--port", "dummy-port", "--json", command],
                {"S3_HIDBOT_SERIAL": "env-port"},
            )
            self.assertEqual(code, 0, command)
            self.assertEqual(errors, "", command)
            value = json.loads(output)
            self.assertEqual(value["desired"], desired, command)
            self.assertEqual(value["observed"], observed, command)
            self.assertEqual(
                self.wire_commands(calls), ["protocol.hello", wire_command], command
            )

    def test_hid_route_operator_commands_are_explicit(self) -> None:
        for argv, wire_command, desired in (
            (["hid-route-status"], "hid.route.status", "none"),
            (["hid-route-set", "none"], "hid.route.set", "none"),
            (["hid-route-set", "usb"], "hid.route.set", "usb"),
        ):
            code, output, errors, calls = self.run_cli(
                ["--port", "dummy-port", "--json", *argv],
                {"S3_HIDBOT_SERIAL": "env-port"},
            )
            self.assertEqual(code, 0, argv)
            self.assertEqual(errors, "", argv)
            value = json.loads(output)
            self.assertEqual(value["desired"], desired)
            self.assertEqual(value["active"], desired)
            self.assertEqual(
                self.wire_commands(calls), ["protocol.hello", wire_command]
            )

    def test_self_test_runs_one_safe_session_in_wire_order(self) -> None:
        code, output, errors, calls = self.run_cli(
            ["--port", "dummy-port", "self-test"],
            {"S3_HIDBOT_SERIAL": "env-port"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        value = json.loads(output)
        self.assertEqual(value["hello"]["session"], TOKEN)
        self.assertEqual(len(value["hello"]["client_nonce"]), 32)
        self.assertTrue(all(char in "0123456789abcdef" for char in value["hello"]["client_nonce"]))
        self.assertEqual(value["ping"], {"pong": True})
        self.assertEqual(value["info"], {"project": "s3-hidbot"})
        self.assertEqual(value["usb_status"], {"mounted": True})
        self.assertEqual(
            value["release_all"], {"keyboard": "already_up", "mouse": "submitted"}
        )
        commands = self.wire_commands(calls)
        self.assertNotIn("hid.keyboard.report", commands)
        self.assertNotIn("hid.mouse.report", commands)
        self.assertEqual(
            commands,
            [
                "protocol.hello",
                "system.ping",
                "system.info",
                "usb.status",
                "hid.release_all",
            ],
        )
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_accepts_a_verified_bundle_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with patch("hidbot.cli.verify_bundle_directory", return_value=self.verified_manifest()):
                code, _, errors, calls = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", temporary],
                    configure_transport=self.configure_identity_transport,
                )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])

    def test_self_test_json_is_compact_and_contains_nested_results(self) -> None:
        code, output, errors, calls = self.run_cli(
            ["--port", "dummy-port", "--json", "self-test"],
            {"S3_HIDBOT_SERIAL": "env-port"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(output.count("\n"), 1)
        self.assertNotIn("\n\n", output)
        value = json.loads(output)
        self.assertIn("hello", value)
        self.assertIn("release_all", value)
        self.assertEqual(self.wire_commands(calls)[1:], [
            "system.ping",
            "system.info",
            "usb.status",
            "hid.release_all",
        ])

    def test_self_test_fails_fast_without_later_steps_or_reconnect(self) -> None:
        calls: list[tuple[object, ...]] = []

        class PingErrorTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                if data == TRANSPORT_SYNC:
                    self.calls.append(("write", data))
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                    return
                self.calls.append(("write", data))
                if value["cmd"] == "system.ping":
                    self.chunks.append(
                        response(
                            value["id"],
                            TOKEN,
                            error={"code": "UNKNOWN_COMMAND", "message": "ping failed"},
                        )
                    )

        transport = PingErrorTransport(calls)
        errors = io.StringIO()

        def factory(*args: object, **kwargs: object) -> PingErrorTransport:
            calls.append(("factory", args, kwargs))
            return transport

        code = main(
            ["--port", "dummy-port", "self-test"],
            transport_factory=factory,
            output=io.StringIO(),
            error_output=errors,
        )
        self.assertEqual(code, 5)
        self.assertIn("UNKNOWN_COMMAND", errors.getvalue())
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.ping"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "construct" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_global_options_before_and_after_commands(self) -> None:
        for argv in (
            ["--json", "ping"],
            ["ping", "--json"],
            ["--port", "dummy-port", "ping"],
            ["ping", "--port", "dummy-port"],
            ["--timeout", "1", "ping"],
            ["ping", "--timeout", "1"],
        ):
            code, output, errors, _ = self.run_cli(argv, {"S3_HIDBOT_SERIAL": "env-port"})
            self.assertEqual(code, 0, argv)
            self.assertEqual(errors, "", argv)
            self.assertTrue(output.strip(), argv)

        code, _, errors, calls = self.run_cli(
            ["--port", "first-port", "ping", "--port", "last-port"], {}
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        factory_call = next(call for call in calls if call[0] == "factory")
        self.assertEqual(factory_call[1], ("last-port", 115200))

    def test_flash_firmware_json_uses_private_executor_and_env_port(self) -> None:
        code, output, errors, calls = self.run_flash_cli(
            ["--json", "flash-firmware", "bundle-dir"],
            env={"S3_HIDBOT_SERIAL": "env-port", "S3_HIDBOT_BAUD": "not-used"},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        value = json.loads(output)
        self.assertTrue(value["ok"])
        self.assertEqual(value["classification"], "FLASHED_AND_VERIFIED")
        self.assertEqual(value["flash"]["classification"], "FLASHED")
        self.assertEqual(value["verification"]["classification"], "MATCH")
        self.assertEqual(value["verification"]["boot_id"], TOKEN)
        self.assertEqual(sum(call[0] == "transport" for call in calls), 0)
        flash_call = next(call for call in calls if call[0] == "flash")
        self.assertEqual(flash_call[2], "env-port")
        self.assertTrue(flash_call[3]["json_mode"])

    def test_flash_firmware_normal_output_and_explicit_programming_options_are_rejected(self) -> None:
        code, output, errors, calls = self.run_flash_cli(
            ["--port", "flash-port", "flash-firmware", "bundle-dir"],
            env={"S3_HIDBOT_SERIAL": "env-port", "S3_HIDBOT_BAUD": "invalid"},
        )
        self.assertEqual(code, 0)
        self.assertIn("firmware flash: FLASHED", output)
        self.assertIn("post-flash verification: MATCH", output)
        self.assertIn("chip: esp32s3", output)
        self.assertEqual(errors, "")
        self.assertEqual(sum(call[0] == "transport" for call in calls), 0)

        for option, value in (("--baud", "9600"), ("--timeout", "4"), ("--attempts", "1")):
            for placement in (
                [option, value, "flash-firmware", "bundle-dir"],
                ["flash-firmware", option, value, "bundle-dir"],
            ):
                with self.subTest(option=option, placement=placement):
                    calls: list[tuple[object, ...]] = []
                    with patch("hidbot.cli.stage_and_verify_firmware_bundle") as stage:
                        with patch("hidbot.cli.resolve_port", side_effect=AssertionError("port resolved")):
                            with self.assertRaises(SystemExit) as raised:
                                main(
                                    placement,
                                    environ={"S3_HIDBOT_SERIAL": "env-port"},
                                    transport_factory=lambda *args, **kwargs: calls.append((args, kwargs)),
                                    output=io.StringIO(),
                                    error_output=io.StringIO(),
                                )
                    self.assertEqual(raised.exception.code, 2)
                    stage.assert_not_called()
                    self.assertEqual(calls, [])

    def test_flash_execution_failure_maps_to_exit_eight_without_stdout(self) -> None:
        def fail(*args: object, **kwargs: object) -> FlashExecutionResult:
            del args, kwargs
            raise FlashExecutionError(
                "esptool failed after 3 attempts",
                attempts=3,
                diagnostic_tail=b"bounded esptool diagnostic",
            )

        code, output, errors, calls = self.run_flash_cli(
            ["--port", "flash-port", "--json", "flash-firmware", "bundle-dir"],
            executor=fail,
        )
        self.assertEqual(code, 8)
        self.assertEqual(output, "")
        self.assertIn("bounded esptool diagnostic", errors)
        self.assertEqual(sum(call[0] == "transport" for call in calls), 0)

    def test_flash_post_verification_failures_retain_flashed_phase_and_exit_mapping(self) -> None:
        cases = (
            (VerificationPhaseClassification.MISMATCH, 7),
            (VerificationPhaseClassification.IDENTITY_UNAVAILABLE, 7),
            (VerificationPhaseClassification.TRANSPORT_UNAVAILABLE, 3),
            (VerificationPhaseClassification.TIMEOUT, 6),
            (VerificationPhaseClassification.PROTOCOL_ERROR, 4),
            (VerificationPhaseClassification.COMPATIBILITY_ERROR, 4),
            (VerificationPhaseClassification.REMOTE_ERROR, 5),
        )
        for classification, expected_exit in cases:
            with self.subTest(classification=classification):
                def workflow_runner(bundle: object, port: str, **kwargs: object) -> ProvisioningWorkflowResult:
                    flash = kwargs["flash_executor"](
                        bundle,
                        port,
                        json_mode=kwargs["json_mode"],
                        on_retry=kwargs["on_retry"],
                    )
                    result = self.flash_workflow_result(bundle.artifact_identity, classification)
                    return ProvisioningWorkflowResult(flash=flash, verification=result.verification)

                code, output, errors, calls = self.run_flash_cli(
                    ["--json", "--port", "flash-port", "flash-firmware", "bundle-dir"],
                    workflow_runner=workflow_runner,
                )
                self.assertEqual(code, expected_exit)
                self.assertEqual(errors, "")
                value = json.loads(output)
                self.assertFalse(value["ok"])
                self.assertEqual(value["classification"], "FLASHED_VERIFICATION_FAILED")
                self.assertEqual(value["flash"]["classification"], "FLASHED")
                self.assertEqual(value["verification"]["classification"], classification.value)
                self.assertEqual(sum(call[0] == "flash" for call in calls), 1)
                self.assertEqual(sum(call[0] == "transport" for call in calls), 0)

    def test_flash_remote_error_human_output_retains_successful_programming(self) -> None:
        def workflow_runner(bundle: object, port: str, **kwargs: object) -> ProvisioningWorkflowResult:
            flash = kwargs["flash_executor"](
                bundle,
                port,
                json_mode=kwargs["json_mode"],
                on_retry=kwargs["on_retry"],
            )
            result = self.flash_workflow_result(
                bundle.artifact_identity,
                VerificationPhaseClassification.REMOTE_ERROR,
            )
            return ProvisioningWorkflowResult(flash=flash, verification=result.verification)

        code, output, errors, calls = self.run_flash_cli(
            ["--port", "flash-port", "flash-firmware", "bundle-dir"],
            workflow_runner=workflow_runner,
        )
        self.assertEqual(code, 5)
        self.assertEqual(errors, "")
        self.assertIn("firmware flash: FLASHED", output)
        self.assertIn("post-flash verification: REMOTE_ERROR", output)
        self.assertEqual(sum(call[0] == "flash" for call in calls), 1)
        self.assertEqual(sum(call[0] == "transport" for call in calls), 0)

    def test_flash_mismatch_human_output_retains_successful_programming(self) -> None:
        def workflow_runner(bundle: object, port: str, **kwargs: object) -> ProvisioningWorkflowResult:
            flash = kwargs["flash_executor"](
                bundle,
                port,
                json_mode=kwargs["json_mode"],
                on_retry=kwargs["on_retry"],
            )
            result = self.flash_workflow_result(
                bundle.artifact_identity,
                VerificationPhaseClassification.MISMATCH,
            )
            return ProvisioningWorkflowResult(flash=flash, verification=result.verification)

        code, output, errors, _ = self.run_flash_cli(
            ["--port", "flash-port", "flash-firmware", "bundle-dir"],
            workflow_runner=workflow_runner,
        )
        self.assertEqual(code, 7)
        self.assertEqual(errors, "")
        self.assertIn("firmware flash: FLASHED", output)
        self.assertIn("post-flash verification: MISMATCH", output)
        self.assertIn("mismatch: TARGET_MISMATCH", output)

    def test_flash_artifact_failure_happens_before_programming_or_transport(self) -> None:
        calls: list[object] = []

        def executor(*args: object, **kwargs: object) -> FlashExecutionResult:
            del args, kwargs
            calls.append("flash")
            raise AssertionError("artifact failure must prevent flashing")

        def transport_factory(*args: object, **kwargs: object) -> object:
            del args, kwargs
            calls.append("transport")
            raise AssertionError("artifact failure must prevent serial access")

        output = io.StringIO()
        errors = io.StringIO()
        with patch(
            "hidbot.cli.stage_and_verify_firmware_bundle",
            side_effect=ArtifactError("invalid artifact"),
        ):
            code = main(
                ["--port", "flash-port", "flash-firmware", "bundle-dir"],
                transport_factory=transport_factory,
                flash_executor=executor,
                output=output,
                error_output=errors,
            )
        self.assertEqual(code, 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("artifact error: invalid artifact", errors.getvalue())
        self.assertEqual(calls, [])

    @staticmethod
    def verified_manifest() -> dict[str, object]:
        return {
            "project": "s3-hidbot",
            "firmware": {
                "target": "esp32s3",
                "protocol_version": 1,
                "version": "0.1.0-dev",
                "source_revision": "a" * 40,
                "build_profile": "freenove-fnk0085",
                "idf_version": "v5.5.4",
            },
            "runtime_identity": {"app_elf_sha256": "b" * 64},
        }

    @staticmethod
    def identity_info(**overrides: object) -> dict[str, object]:
        value: dict[str, object] = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
            "firmware": {
                "version": "0.1.0-dev",
                "source_revision": "a" * 40,
                "app_elf_sha256": "b" * 64,
                "build_profile": "freenove-fnk0085",
            },
        }
        for key, item in overrides.items():
            if key in {"version", "source_revision", "app_elf_sha256", "build_profile"}:
                firmware = value["firmware"]
                assert isinstance(firmware, dict)
                firmware[key] = item
            else:
                value[key] = item
        return value

    @staticmethod
    def configure_identity_transport(
        transport: FakeTransport, info: object | None = None, *, identity_capability: bool = True
    ) -> None:
        if identity_capability:
            transport.hello_capabilities.append("firmware.identity-v1")
        transport.info_result = CliTests.identity_info() if info is None else info

    @staticmethod
    def validated_identity_info(value: dict[str, object]) -> SystemInfo:
        firmware_value = value["firmware"]
        assert isinstance(firmware_value, dict)
        return SystemInfo(
            project=str(value["project"]),
            target=str(value["target"]),
            idf_version=str(value["idf_version"]),
            protocol_version=int(value["protocol_version"]),
            firmware=FirmwareIdentity(
                version=str(firmware_value["version"]),
                source_revision=firmware_value["source_revision"],
                app_elf_sha256=str(firmware_value["app_elf_sha256"]),
                build_profile=str(firmware_value["build_profile"]),
            ),
        )

    @staticmethod
    def build_verified_artifact_archive(root: Path) -> tuple[Path, str]:
        """Build a minimal archive fixture with the canonical artifact primitives."""

        bundle = root / "s3-hidbot-firmware-0.1.0-dev-esp32s3-freenove-fnk0085"
        payloads = {
            "application.bin": b"application image\n",
            "application.elf": b"exact linked elf\n",
            "bootloader/bootloader.bin": b"bootloader image\n",
            "partition_table/partition-table.bin": b"partition image\n",
            "provenance/sdkconfig": b"CONFIG_APP_REPRODUCIBLE_BUILD=y\n",
            "provenance/dependencies.lock": b"version: 5.5.4\ntarget: esp32s3\n",
            "LICENSE": b"MIT License\n",
        }
        for relative, payload in payloads.items():
            path = bundle / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        flash_plan: dict[str, Any] = {
            "write_flash_args": [
                "--flash_mode",
                "dio",
                "--flash_size",
                "2MB",
                "--flash_freq",
                "80m",
            ],
            "flash_settings": {
                "flash_mode": "dio",
                "flash_size": "2MB",
                "flash_freq": "80m",
            },
            "flash_files": {
                "0x0": "bootloader/bootloader.bin",
                "0x8000": "partition_table/partition-table.bin",
                "0x10000": "application.bin",
            },
            "bootloader": {
                "offset": "0x0",
                "file": "bootloader/bootloader.bin",
                "encrypted": "false",
            },
            "app": {"offset": "0x10000", "file": "application.bin", "encrypted": "false"},
            "partition-table": {
                "offset": "0x8000",
                "file": "partition_table/partition-table.bin",
                "encrypted": "false",
            },
            "extra_esptool_args": {
                "before": "default_reset",
                "after": "hard_reset",
                "stub": True,
                "chip": "esp32s3",
            },
        }
        write_deterministic_json(bundle / "flasher_args.json", flash_plan)
        roles = {
            "application.elf": "application_elf",
            "application.bin": "application_bin",
            "bootloader/bootloader.bin": "bootloader_bin",
            "partition_table/partition-table.bin": "partition_table_bin",
            "flasher_args.json": "flash_plan",
            "provenance/sdkconfig": "effective_sdkconfig",
            "provenance/dependencies.lock": "dependencies_lock",
            "LICENSE": "license",
        }
        files = {
            relative: {"sha256": sha256_file(bundle / relative), "role": role}
            for relative, role in roles.items()
        }
        manifest: dict[str, Any] = {
            "artifact_manifest_version": 1,
            "project": "s3-hidbot",
            "firmware": {
                "version": "0.1.0-dev",
                "protocol_version": 1,
                "source_revision": "a" * 40,
                "target": "esp32s3",
                "build_profile": "freenove-fnk0085",
                "idf_version": "v5.5.4",
            },
            "runtime_identity": {"app_elf_sha256": files["application.elf"]["sha256"]},
            "build": {
                "reproducible": True,
                "source_date_epoch": 0,
                "container_image": None,
                "tools": {
                    "compiler": "14.2.0",
                    "cmake": "3.30.2",
                    "ninja": "1.12.1",
                    "python": "3.12.3",
                    "esptool": "4.12.dev1",
                },
            },
            "provenance": {
                "dependencies_lock_sha256": files["provenance/dependencies.lock"]["sha256"],
                "effective_sdkconfig_sha256": files["provenance/sdkconfig"]["sha256"],
            },
            "flash_plan": "flasher_args.json",
            "files": files,
        }
        write_deterministic_json(bundle / "manifest.json", manifest)
        checksum_paths = sorted(["manifest.json", *files])
        (bundle / "SHA256SUMS").write_text(
            "".join(f"{sha256_file(bundle / relative)}  {relative}\n" for relative in checksum_paths),
            encoding="ascii",
        )
        archive = root / "verified-artifact.tar.gz"
        create_deterministic_tar_gz(bundle, archive, 0)
        return archive, str(files["application.elf"]["sha256"])

    def test_verify_firmware_rejects_corrupt_artifact_before_transport(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            artifact = Path(temporary) / "corrupt.tar.gz"
            artifact.write_bytes(b"not a firmware bundle")
            calls: list[tuple[object, ...]] = []

            def factory(*args: object, **kwargs: object) -> FakeTransport:
                calls.append(("factory", args, kwargs))
                return FakeTransport(calls)

            output = io.StringIO()
            errors = io.StringIO()
            code = main(
                ["verify-firmware", str(artifact)],
                environ={"S3_HIDBOT_SERIAL": "env-port"},
                transport_factory=factory,
                output=output,
                error_output=errors,
            )
        self.assertEqual(code, 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("artifact error:", errors.getvalue())
        self.assertFalse(any(call[0] == "factory" for call in calls))
        self.assertFalse(any(call[0] == "construct" for call in calls))
        self.assertFalse(any(call[0] == "open" for call in calls))

    def test_verify_artifact_valid_archive_and_directory_are_serial_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, elf_sha256 = self.build_verified_artifact_archive(root)
            directory = next(root.glob("s3-hidbot-firmware-*"))
            archive_code, archive_output, archive_errors, archive_calls = self.run_artifact_only(
                ["--json", "verify-artifact", str(archive)]
            )
            directory_code, directory_output, directory_errors, directory_calls = self.run_artifact_only(
                ["--json", "verify-artifact", str(directory)]
            )

        self.assertEqual(archive_code, 0)
        self.assertEqual(directory_code, 0)
        self.assertEqual(archive_errors, "")
        self.assertEqual(directory_errors, "")
        self.assertEqual(archive_calls, [])
        self.assertEqual(directory_calls, [])
        self.assertEqual(archive_output.count("\n"), 1)
        self.assertEqual(directory_output.count("\n"), 1)
        archive_value = json.loads(archive_output)
        directory_value = json.loads(directory_output)
        self.assertEqual(archive_value, directory_value)
        self.assertEqual(
            archive_value,
            {
                "ok": True,
                "classification": "VALID",
                "artifact": {
                    "project": "s3-hidbot",
                    "target": "esp32s3",
                    "protocol_version": 1,
                    "version": "0.1.0-dev",
                    "source_revision": "a" * 40,
                    "app_elf_sha256": elf_sha256,
                    "build_profile": "freenove-fnk0085",
                    "idf_version": "v5.5.4",
                },
            },
        )

    def test_verify_artifact_human_output_and_port_are_serial_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive, _ = self.build_verified_artifact_archive(Path(temporary))
            for option, value in (
                ("--port", "definitely-invalid"),
                ("--baud", "230400"),
                ("--timeout", "2"),
                ("--attempts", "4"),
            ):
                for argv in (
                    [option, value, "verify-artifact", str(archive)],
                    ["verify-artifact", option, value, str(archive)],
                ):
                    with self.subTest(argv=argv):
                        code, output, errors, calls = self.run_artifact_only(argv)
                        self.assertEqual(code, 0)
                        self.assertEqual(errors, "")
                        self.assertEqual(calls, [])
                        self.assertTrue(output.startswith("firmware artifact: VALID\nartifact:\n"))
                        self.assertIn('"project": "s3-hidbot"', output)

    def test_verify_artifact_missing_malformed_and_invalid_fail_before_transport(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / "missing-artifact"
            malformed = root / "malformed.tar.gz"
            malformed.write_bytes(b"not a firmware artifact")
            _, _ = self.build_verified_artifact_archive(root)
            invalid_directory = next(root.glob("s3-hidbot-firmware-*"))
            (invalid_directory / "manifest.json").write_text("{}\n", encoding="utf-8")
            cases = {
                "missing": str(missing),
                "malformed": str(malformed),
                "invalid": str(invalid_directory),
            }
            results = {
                name: self.run_artifact_only(["verify-artifact", artifact])
                for name, artifact in cases.items()
            }

        for name, (code, output, errors, calls) in results.items():
            with self.subTest(name=name):
                self.assertEqual(code, 2)
                self.assertEqual(output, "")
                self.assertIn("artifact error:", errors)
                self.assertEqual(calls, [])

    def test_verify_firmware_real_archive_matches_fake_device_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive, elf_sha256 = self.build_verified_artifact_archive(Path(temporary))
            calls: list[tuple[object, ...]] = []
            output = io.StringIO()
            errors = io.StringIO()

            def factory(*args: object, **kwargs: object) -> FakeTransport:
                calls.append(("factory", args, kwargs))
                transport = FakeTransport(calls)
                self.configure_identity_transport(
                    transport, self.identity_info(app_elf_sha256=elf_sha256)
                )
                return transport

            code = main(
                ["--port", "dummy-port", "--json", "verify-firmware", str(archive)],
                transport_factory=factory,
                output=output,
                error_output=errors,
            )

        self.assertEqual(code, 0)
        self.assertEqual(errors.getvalue(), "")
        value = json.loads(output.getvalue())
        self.assertEqual(
            set(value),
            {
                "ok",
                "match",
                "classification",
                "artifact",
                "device",
                "mismatches",
                "unavailable_reason",
            },
        )
        self.assertTrue(value["ok"])
        self.assertTrue(value["match"])
        self.assertEqual(value["classification"], "MATCH")
        self.assertEqual(value["artifact"]["app_elf_sha256"], elf_sha256)
        self.assertEqual(value["device"]["app_elf_sha256"], elf_sha256)
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "construct" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)
        commands = self.wire_commands(calls)
        self.assertFalse(
            {"hid.release_all", "hid.keyboard.report", "hid.mouse.report", "usb.status"}
            & set(commands)
        )

    def test_verify_firmware_match_uses_only_hello_and_info(self) -> None:
        with tempfile.NamedTemporaryFile() as artifact:
            with patch("hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()):
                code, output, errors, calls = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", artifact.name, "--json"],
                    configure_transport=self.configure_identity_transport,
                )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(
            output,
            '{"artifact":{"app_elf_sha256":"' + "b" * 64
            + '","build_profile":"freenove-fnk0085","idf_version":"v5.5.4","project":"s3-hidbot","protocol_version":1,"source_revision":"'
            + "a" * 40
            + '","target":"esp32s3","version":"0.1.0-dev"},"classification":"MATCH","device":{"app_elf_sha256":"'
            + "b" * 64
            + '","build_profile":"freenove-fnk0085","idf_version":"v5.5.4","project":"s3-hidbot","protocol_version":1,"source_revision":"'
            + "a" * 40
            + '","target":"esp32s3","version":"0.1.0-dev"},"match":true,"mismatches":[],"ok":true,"unavailable_reason":null}\n',
        )
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_transport_error_closes_without_requests(self) -> None:
        calls: list[tuple[object, ...]] = []

        class FailedOpenTransport(FakeTransport):
            def open(self) -> None:
                self.calls.append(("open",))
                raise TransportError("open failed")

        transport = FailedOpenTransport(calls)
        with tempfile.NamedTemporaryFile() as artifact, patch(
            "hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()
        ):
            code, output, errors, calls = self.run_cli_with_transport(
                ["--port", "dummy-port", "verify-firmware", artifact.name], transport
            )
        self.assertEqual(code, 3)
        self.assertEqual(output, "")
        self.assertIn("open failed", errors)
        self.assertEqual(self.wire_commands(calls), [])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_info_timeout_closes_without_reconnect(self) -> None:
        calls: list[tuple[object, ...]] = []

        class SilentInfoTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                if data == TRANSPORT_SYNC:
                    self.calls.append(("write", data))
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                    return
                self.calls.append(("write", data))

        transport = SilentInfoTransport(calls)
        with tempfile.NamedTemporaryFile() as artifact, patch(
            "hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()
        ):
            code, output, errors, calls = self.run_cli_with_transport(
                [
                    "--port",
                    "dummy-port",
                    "--timeout",
                    "0.001",
                    "--attempts",
                    "1",
                    "verify-firmware",
                    artifact.name,
                ],
                transport,
            )
        self.assertEqual(code, 6)
        self.assertEqual(output, "")
        self.assertIn("timed out", errors)
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_info_remote_error_closes_without_reconnect(self) -> None:
        calls: list[tuple[object, ...]] = []

        class InfoRemoteErrorTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                if data == TRANSPORT_SYNC:
                    self.calls.append(("write", data))
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                    return
                self.calls.append(("write", data))
                self.chunks.append(
                    response(
                        value["id"],
                        TOKEN,
                        error={"code": "SYSTEM_INFO_FAILED", "message": "info failed"},
                    )
                )

        transport = InfoRemoteErrorTransport(calls)
        with tempfile.NamedTemporaryFile() as artifact, patch(
            "hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()
        ):
            code, output, errors, calls = self.run_cli_with_transport(
                ["--port", "dummy-port", "verify-firmware", artifact.name], transport
            )
        self.assertEqual(code, 5)
        self.assertEqual(output, "")
        self.assertIn("SYSTEM_INFO_FAILED", errors)
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_info_session_loss_closes_without_reconnect(self) -> None:
        calls: list[tuple[object, ...]] = []

        class InfoSessionLossTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                if data == TRANSPORT_SYNC:
                    self.calls.append(("write", data))
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                    return
                self.calls.append(("write", data))
                self.chunks.append(
                    response(
                        value["id"],
                        None,
                        error={"code": "SESSION_MISMATCH", "message": "session lost"},
                    )
                )

        transport = InfoSessionLossTransport(calls)
        with tempfile.NamedTemporaryFile() as artifact, patch(
            "hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()
        ):
            code, output, errors, calls = self.run_cli_with_transport(
                ["--port", "dummy-port", "verify-firmware", artifact.name], transport
            )
        self.assertEqual(code, 4)
        self.assertEqual(output, "")
        self.assertIn("session loss", errors)
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "factory" for call in calls), 1)
        self.assertEqual(sum(call[0] == "open" for call in calls), 1)
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_reports_all_mismatch_codes_in_fixed_order(self) -> None:
        cases = (
            ("project", "other", "PROJECT_MISMATCH"),
            ("target", "esp32c6", "TARGET_MISMATCH"),
            ("protocol_version", 2, "PROTOCOL_VERSION_MISMATCH"),
            ("version", "0.2.0", "FIRMWARE_VERSION_MISMATCH"),
            ("source_revision", "c" * 40, "SOURCE_REVISION_MISMATCH"),
            ("app_elf_sha256", "c" * 64, "ELF_SHA256_MISMATCH"),
            ("build_profile", "other-board", "BUILD_PROFILE_MISMATCH"),
            ("idf_version", "v5.6.0", "IDF_VERSION_MISMATCH"),
        )
        with tempfile.NamedTemporaryFile() as artifact:
            for field, replacement, expected in cases:
                info = self.identity_info(**{field: replacement})
                validation = (
                    patch("hidbot.cli.validate_system_info", return_value=self.validated_identity_info(info))
                    if field in {"project", "protocol_version"}
                    else contextlib.nullcontext()
                )
                with self.subTest(field=field), patch(
                    "hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()
                ), validation:
                    code, output, errors, calls = self.run_cli(
                        ["--port", "dummy-port", "verify-firmware", artifact.name, "--json"],
                        configure_transport=lambda transport, field=field, replacement=replacement: self.configure_identity_transport(
                            transport, self.identity_info(**{field: replacement})
                        ),
                    )
                self.assertEqual(code, 7)
                self.assertEqual(errors, "")
                self.assertEqual(json.loads(output)["mismatches"], [expected])
                self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
                self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_keeps_multiple_mismatches_and_renders_human_diagnostics(self) -> None:
        with tempfile.NamedTemporaryFile() as artifact:
            info = self.identity_info(project="other", target="esp32c6", version="0.2.0")
            with patch("hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()), patch(
                "hidbot.cli.validate_system_info", return_value=self.validated_identity_info(info)
            ):
                code, output, errors, _ = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", artifact.name],
                    configure_transport=lambda transport: self.configure_identity_transport(
                        transport, info
                    ),
                )
        self.assertEqual(code, 7)
        self.assertEqual(errors, "")
        self.assertIn("firmware identity: MISMATCH", output)
        self.assertIn("mismatch: PROJECT_MISMATCH", output)
        self.assertIn("mismatch: TARGET_MISMATCH", output)
        self.assertIn("mismatch: FIRMWARE_VERSION_MISMATCH", output)

    def test_verify_firmware_identity_unavailable_preserves_base_diagnostics(self) -> None:
        with tempfile.NamedTemporaryFile() as artifact:
            with patch("hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()):
                code, output, errors, calls = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", artifact.name, "--json"],
                    configure_transport=lambda transport: self.configure_identity_transport(
                        transport,
                        {"project": "s3-hidbot", "target": "esp32s3", "idf_version": "v5.5.4", "protocol_version": 1},
                        identity_capability=False,
                    ),
                )
        self.assertEqual(code, 7)
        self.assertEqual(errors, "")
        value = json.loads(output)
        self.assertEqual(value["classification"], "IDENTITY_UNAVAILABLE")
        self.assertEqual(value["unavailable_reason"], "FIRMWARE_IDENTITY_CAPABILITY_MISSING")
        self.assertEqual(value["mismatches"], [])
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])

    def test_verify_firmware_null_revision_is_identity_unavailable(self) -> None:
        with tempfile.NamedTemporaryFile() as artifact:
            with patch("hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()):
                code, output, errors, calls = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", artifact.name, "--json"],
                    configure_transport=lambda transport: self.configure_identity_transport(
                        transport, self.identity_info(source_revision=None)
                    ),
                )
        self.assertEqual(code, 7)
        self.assertEqual(errors, "")
        self.assertEqual(
            json.loads(output)["unavailable_reason"], "SOURCE_REVISION_UNAVAILABLE"
        )
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_verify_firmware_rejects_malformed_advertised_identity_as_protocol_error(self) -> None:
        with tempfile.NamedTemporaryFile() as artifact:
            with patch("hidbot.cli.verify_bundle_archive", return_value=self.verified_manifest()):
                code, output, errors, calls = self.run_cli(
                    ["--port", "dummy-port", "verify-firmware", artifact.name],
                    configure_transport=lambda transport: self.configure_identity_transport(
                        transport,
                        {"project": "s3-hidbot", "target": "esp32s3", "idf_version": "v5.5.4", "protocol_version": 1},
                    ),
                )
        self.assertEqual(code, 4)
        self.assertEqual(output, "")
        self.assertIn("firmware.identity-v1", errors)
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "system.info"])
        self.assertEqual(sum(call[0] == "close" for call in calls), 1)

    def test_unsafe_options_are_command_local(self) -> None:
        for argv in (
            ["ping", "--unsafe-hid"],
            ["--unsafe-hid", "ping"],
            ["release-all", "--modifiers", "0"],
            ["info", "--x", "1"],
            ["self-test", "--unsafe-hid"],
        ):
            code, errors, calls = self.run_parser_error(argv)
            self.assertEqual(code, 2, argv)
            self.assertIn("usage:", errors, argv)
            self.assertEqual(calls, [], argv)

    def test_invalid_keyboard_inputs_never_construct_transport(self) -> None:
        cases = [
            ["keyboard-report", "--modifiers", "0"],
            ["keyboard-report", "--unsafe-hid", "--modifiers", "256"],
            [
                "keyboard-report",
                "--unsafe-hid",
                "--modifiers",
                "0",
                "--key",
                "0x04",
                "--key",
                "0x04",
            ],
            [
                "keyboard-report",
                "--unsafe-hid",
                "--modifiers",
                "0",
                "--key",
                "0x05",
                "--key",
                "0x04",
            ],
            ["keyboard-report", "--unsafe-hid", "--modifiers", "0", "--key", "0x01"],
            [
                "keyboard-report",
                "--unsafe-hid",
                "--modifiers",
                "0",
                "--key",
                "4",
                "--key",
                "5",
                "--key",
                "6",
                "--key",
                "7",
                "--key",
                "8",
                "--key",
                "9",
                "--key",
                "10",
            ],
        ]
        for argv in cases:
            code, errors, calls = self.run_parser_error(argv)
            self.assertEqual(code, 2, argv)
            self.assertTrue(errors, argv)
            self.assertEqual(calls, [], argv)

    def test_invalid_mouse_inputs_never_construct_transport(self) -> None:
        full = [
            "mouse-report",
            "--unsafe-hid",
            "--buttons",
            "0",
            "--x",
            "0",
            "--y",
            "0",
            "--wheel",
            "0",
            "--pan",
            "0",
        ]
        for missing in ("--unsafe-hid", "--buttons", "--x", "--y", "--wheel", "--pan"):
            index = full.index(missing)
            argv = full[:index] + full[index + (1 if missing == "--unsafe-hid" else 2) :]
            code, _, calls = self.run_parser_error(argv)
            self.assertEqual(code, 2, missing)
            self.assertEqual(calls, [], missing)

        invalid_ranges = [("--buttons", value) for value in ("-1", "32")]
        invalid_ranges.extend(
            (option, value)
            for option in ("--x", "--y", "--wheel", "--pan")
            for value in ("-128", "128")
        )
        for option, value in invalid_ranges:
            argv = [
                "mouse-report",
                "--unsafe-hid",
                "--buttons",
                "0",
                "--x",
                "0",
                "--y",
                "0",
                "--wheel",
                "0",
                "--pan",
                "0",
            ]
            index = argv.index(option)
            argv[index + 1] = value
            code, _, calls = self.run_parser_error(argv)
            self.assertEqual(code, 2, (option, value))
            self.assertEqual(calls, [], (option, value))

    def test_keyboard_report_dispatch_and_raw_decimal_hex_inputs(self) -> None:
        code, output, errors, calls = self.run_cli(
            [
                "--port",
                "dummy-port",
                "keyboard-report",
                "--unsafe-hid",
                "--modifiers",
                "0xff",
                "--key",
                "4",
                "--key",
                "0x05",
            ],
            {},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(output, '{\n  "state": "submitted"\n}\n')
        self.assertEqual(json.loads(output), {"state": "submitted"})
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "hid.keyboard.report"])
        report_call = next(
            call
            for call in reversed(calls)
            if call[0] == "write" and call[1] != TRANSPORT_SYNC
        )
        report = json.loads(report_call[1][len(FRAME_PREFIX) : -1])
        self.assertEqual(report["params"], {"modifiers": 255, "keys": [4, 5]})

    def test_keyboard_report_already_set_result(self) -> None:
        code, output, errors, calls = self.run_cli(
            ["--port", "dummy-port", "keyboard-report", "--unsafe-hid", "--modifiers", "0"],
            {},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(json.loads(output), {"state": "already_set"})
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "hid.keyboard.report"])

    def test_mouse_report_dispatch_json_and_no_release_cleanup(self) -> None:
        code, output, errors, calls = self.run_cli(
            [
                "--port",
                "dummy-port",
                "--json",
                "mouse-report",
                "--unsafe-hid",
                "--buttons",
                "31",
                "--x",
                "127",
                "--y",
                "-127",
                "--wheel",
                "127",
                "--pan",
                "-127",
            ],
            {},
        )
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(output, '{"state":"submitted"}\n')
        self.assertEqual(self.wire_commands(calls), ["protocol.hello", "hid.mouse.report"])
        report_call = next(
            call
            for call in reversed(calls)
            if call[0] == "write" and call[1] != TRANSPORT_SYNC
        )
        report = json.loads(report_call[1][len(FRAME_PREFIX) : -1])
        self.assertEqual(
            report["params"], {"buttons": 31, "x": 127, "y": -127, "wheel": 127, "pan": -127}
        )

    def test_primitive_remote_error_uses_existing_exit_code(self) -> None:
        calls: list[tuple[object, ...]] = []

        class PrimitiveErrorTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                self.calls.append(("write", data))
                if data == TRANSPORT_SYNC:
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                else:
                    self.chunks.append(
                        response(
                            value["id"],
                            TOKEN,
                            error={"code": "HID_NOT_READY", "message": "not ready"},
                        )
                    )

        transport = PrimitiveErrorTransport(calls)
        errors = io.StringIO()
        code = main(
            [
                "--port",
                "dummy-port",
                "keyboard-report",
                "--unsafe-hid",
                "--modifiers",
                "0",
            ],
            transport_factory=lambda *args, **kwargs: transport,
            output=io.StringIO(),
            error_output=errors,
        )
        self.assertEqual(code, 5)
        self.assertIn("HID_NOT_READY", errors.getvalue())

    def test_parser_help_is_command_specific(self) -> None:
        helps: dict[tuple[str, ...], str] = {}
        for argv, expected in (
            (["--help"], "keyboard-report"),
            (["keyboard-report", "--help"], "--unsafe-hid"),
            (["mouse-report", "--help"], "--pan"),
            (["release-all", "--help"], "safe all-up"),
            (["self-test", "--help"], "release-all safely"),
            (["verify-firmware", "--help"], "ARTIFACT"),
        ):
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as exited:
                    main(argv, output=stdout, error_output=stderr)
            self.assertEqual(exited.exception.code, 0, argv)
            self.assertIn(expected, stdout.getvalue(), argv)
            helps[tuple(argv)] = stdout.getvalue()

        self.assertNotIn("--unsafe-hid", helps[("--help",)])
        flash_help = helps[("verify-firmware", "--help")]
        self.assertIn("--baud", flash_help)

        for argv in (("flash-firmware", "--help"), ("verify-artifact", "--help")):
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as exited:
                    main(list(argv), output=stdout, error_output=stderr)
            self.assertEqual(exited.exception.code, 0, argv)
            helps[argv] = stdout.getvalue()

        for option in ("--baud", "--timeout", "--attempts"):
            self.assertNotIn(option, helps[("flash-firmware", "--help")])
        for option in ("--port", "--baud", "--timeout", "--attempts"):
            self.assertNotIn(option, helps[("verify-artifact", "--help")])

    def test_missing_port_and_malformed_baud_are_config_errors(self) -> None:
        code, _, errors, _ = self.run_cli(["hello"], {})
        self.assertEqual(code, 2)
        self.assertIn("serial port is required", errors)

        code, _, errors, _ = self.run_cli(["--baud", "bad", "hello"], {"S3_HIDBOT_SERIAL": "env-port"})
        self.assertEqual(code, 2)
        self.assertIn("baud must be a positive integer", errors)

        code, _, errors, _ = self.run_cli(["hello"], {"S3_HIDBOT_SERIAL": "env-port", "S3_HIDBOT_BAUD": "0"})
        self.assertEqual(code, 2)
        self.assertIn("baud must be a positive integer", errors)

    def test_timeout_and_transport_error_exit_codes(self) -> None:
        calls: list[tuple[object, ...]] = []
        output = io.StringIO()
        errors = io.StringIO()

        class SilentTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                self.calls.append(("write", data))

        silent = SilentTransport(calls)
        self.assertEqual(
            main(
                ["--port", "dummy-port", "--timeout", "0.001", "--attempts", "1", "hello"],
                transport_factory=lambda *args, **kwargs: silent,
                output=output,
                error_output=errors,
            ),
            6,
        )
        self.assertIn("timed out", errors.getvalue())

        class FailedOpenTransport(FakeTransport):
            def open(self) -> None:
                raise TransportError("open failed")

        output = io.StringIO()
        errors = io.StringIO()
        failed = FailedOpenTransport([])
        self.assertEqual(
            main(
                ["--port", "dummy-port", "hello"],
                transport_factory=lambda *args, **kwargs: failed,
                output=output,
                error_output=errors,
            ),
            3,
        )
        self.assertIn("open failed", errors.getvalue())

    def test_remote_error_exit_code(self) -> None:
        calls: list[tuple[object, ...]] = []

        class RemoteErrorTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                self.calls.append(("write", data))
                if data == TRANSPORT_SYNC:
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                else:
                    self.chunks.append(
                        response(
                            value["id"],
                            TOKEN,
                            error={"code": "UNKNOWN_COMMAND", "message": "not supported"},
                        )
                    )

        transport = RemoteErrorTransport(calls)
        output = io.StringIO()
        errors = io.StringIO()
        self.assertEqual(
            main(
                ["--port", "dummy-port", "ping"],
                transport_factory=lambda *args, **kwargs: transport,
                output=output,
                error_output=errors,
            ),
            5,
        )
        self.assertIn("UNKNOWN_COMMAND", errors.getvalue())

    def test_release_all_remote_error_uses_existing_exit_code(self) -> None:
        calls: list[tuple[object, ...]] = []

        class ReleaseErrorTransport(FakeTransport):
            def write(self, data: bytes) -> None:
                self.calls.append(("write", data))
                if data == TRANSPORT_SYNC:
                    return
                value = json.loads(data[len(FRAME_PREFIX) : -1])
                if value["cmd"] == "protocol.hello":
                    super().write(data)
                else:
                    self.chunks.append(
                        response(
                            value["id"],
                            TOKEN,
                            error={"code": "HID_SAFETY_PENDING", "message": "pending"},
                        )
                    )

        transport = ReleaseErrorTransport(calls)
        output = io.StringIO()
        errors = io.StringIO()
        self.assertEqual(
            main(
                ["--port", "dummy-port", "release-all"],
                transport_factory=lambda *args, **kwargs: transport,
                output=output,
                error_output=errors,
            ),
            5,
        )
        self.assertIn("HID_SAFETY_PENDING", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
