from __future__ import annotations

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
        for command in ("ping", "info", "usb-status"):
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


if __name__ == "__main__":
    unittest.main()
