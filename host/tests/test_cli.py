from __future__ import annotations

import contextlib
import io
import json
import unittest
from collections import deque

from hidbot.cli import main
from hidbot.errors import TransportError
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC


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
                        "capabilities": [
                            "protocol.hello-v1",
                            "system.ping-v1",
                            "system.info-v1",
                            "usb.status-v1",
                            "hid.lease-v1",
                            "hid.release-all-v1",
                            "hid.keyboard-report-v1",
                            "hid.mouse-report-v1",
                        ],
                    },
                )
            )
        elif value["cmd"] == "system.ping":
            self.chunks.append(response(value["id"], TOKEN, result={"pong": True}))
        elif value["cmd"] == "system.info":
            self.chunks.append(response(value["id"], TOKEN, result={"project": "s3-hidbot"}))
        elif value["cmd"] == "hid.release_all":
            self.chunks.append(
                response(
                    value["id"],
                    TOKEN,
                    result={"keyboard": "already_up", "mouse": "submitted"},
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
    def run_cli(self, argv: list[str], env: dict[str, str] | None = None) -> tuple[int, str, str, list[tuple[object, ...]]]:
        calls: list[tuple[object, ...]] = []
        transport = FakeTransport(calls)

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
        for command in ("ping", "info", "usb-status", "release-all"):
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

    def test_unsafe_options_are_command_local(self) -> None:
        for argv in (
            ["ping", "--unsafe-hid"],
            ["--unsafe-hid", "ping"],
            ["release-all", "--modifiers", "0"],
            ["info", "--x", "1"],
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
        root_help = ""
        for argv, expected in (
            (["--help"], "keyboard-report"),
            (["keyboard-report", "--help"], "--unsafe-hid"),
            (["mouse-report", "--help"], "--pan"),
            (["release-all", "--help"], "safe all-up"),
        ):
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as exited:
                    main(argv, output=stdout, error_output=stderr)
            self.assertEqual(exited.exception.code, 0, argv)
            self.assertIn(expected, stdout.getvalue(), argv)
            if argv == ["--help"]:
                root_help = stdout.getvalue()
        self.assertNotIn("--unsafe-hid", root_help)

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
