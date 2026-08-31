from __future__ import annotations

import unittest
from collections import deque
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Callable

from hidbot.client import HelloResult
from hidbot.errors import (
    CompatibilityError,
    FlashExecutionError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from hidbot.firmware_verification import ArtifactFirmwareIdentity
from hidbot.flashing import FlashExecutionResult
from hidbot.provisioning_workflow import (
    MAX_DRAIN_BYTES,
    VerificationPhaseClassification,
    _drain_to_quiet,
    run_post_flash_provisioning,
)


TOKEN = "0123456789abcdef0123456789abcdef"
CAPABILITIES = (
    "protocol.hello-v1",
    "system.ping-v1",
    "system.info-v1",
    "usb.status-v1",
    "hid.lease-v1",
    "hid.release-all-v1",
    "firmware.identity-v1",
)


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        return self.value

    def sleep(self, duration: float) -> None:
        self.value += duration


class FakeTransport:
    def __init__(
        self,
        *,
        chunks: tuple[bytes, ...] = (),
        open_error: Exception | None = None,
        read_advance: float = 0.0,
        continuous: bytes | None = None,
        clock: FakeClock | None = None,
    ) -> None:
        self.chunks = deque(chunks)
        self.open_error = open_error
        self.read_advance = read_advance
        self.continuous = continuous
        self.clock = clock
        self.opened = False
        self.closed = False
        self.read_calls = 0

    def open(self) -> None:
        if self.open_error is not None:
            raise self.open_error
        self.opened = True

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del timeout
        self.read_calls += 1
        if self.clock is not None:
            self.clock.value += self.read_advance
        if self.chunks:
            chunk = self.chunks.popleft()
            if len(chunk) > max_bytes:
                self.chunks.appendleft(chunk[max_bytes:])
                return chunk[:max_bytes]
            return chunk
        if self.continuous is not None:
            return self.continuous[:max_bytes]
        return b""

    def close(self) -> None:
        self.closed = True


@dataclass
class ClientPlan:
    connect_error: Exception | None = None
    info_error: Exception | None = None
    info_value: object | None = None


class FakeClient:
    def __init__(self, plan: ClientPlan) -> None:
        self.plan = plan
        self.closed = False

    def connect(self) -> HelloResult:
        if self.plan.connect_error is not None:
            raise self.plan.connect_error
        return HelloResult(TOKEN, TOKEN, "f" * 32, CAPABILITIES, 5000)

    def info(self) -> object:
        if self.plan.info_error is not None:
            raise self.plan.info_error
        assert self.plan.info_value is not None
        return self.plan.info_value

    def close(self) -> None:
        self.closed = True


class ProvisioningWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        self.identity = ArtifactFirmwareIdentity(
            project="s3-hidbot",
            target="esp32s3",
            protocol_version=1,
            version="0.1.0-dev",
            source_revision="a" * 40,
            app_elf_sha256="b" * 64,
            build_profile="freenove-fnk0085",
            idf_version="v5.5.4",
        )
        self.bundle = SimpleNamespace(artifact_identity=self.identity)

    def info(self, **changes: object) -> dict[str, object]:
        firmware = {
            "version": "0.1.0-dev",
            "source_revision": "a" * 40,
            "app_elf_sha256": "b" * 64,
            "build_profile": "freenove-fnk0085",
        }
        value: dict[str, object] = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
            "firmware": firmware,
        }
        value.update(changes)
        return value

    def run_workflow(
        self,
        transports: list[FakeTransport],
        plans: list[ClientPlan],
        *,
        flash_error: Exception | None = None,
    ):
        flash_calls: list[object] = []
        clients: list[FakeClient] = []
        client_parameters: list[tuple[float, int]] = []
        transport_factory_calls: list[object] = []
        self.last_flash_calls = flash_calls
        self.last_client_parameters = client_parameters
        self.last_transport_factory_calls = transport_factory_calls

        def flash(*args: object, **kwargs: object) -> FlashExecutionResult:
            flash_calls.append((args, kwargs))
            if flash_error is not None:
                raise flash_error
            return FlashExecutionResult(attempts=1, chip="esp32s3", image_count=3)

        transport_values = iter(transports)
        plan_values = iter(plans)

        def transport_factory(*args: object, **kwargs: object) -> FakeTransport:
            transport_factory_calls.append((args, kwargs))
            return next(transport_values)

        def client_factory(transport: object, timeout: float, attempts: int) -> FakeClient:
            del transport
            client_parameters.append((timeout, attempts))
            client = FakeClient(next(plan_values))
            clients.append(client)
            return client

        result = run_post_flash_provisioning(
            self.bundle,
            "test-port",
            flash_executor=flash,
            transport_factory=transport_factory,
            client_factory=client_factory,
            clock=self.clock,
            sleeper=self.clock.sleep,
        )
        return result, flash_calls, clients

    def ready_transport(self, *chunks: bytes) -> FakeTransport:
        return FakeTransport(chunks=chunks, clock=self.clock)

    def test_flash_success_clean_uart_match_and_cleanup(self) -> None:
        transport = self.ready_transport()
        result, flashes, clients = self.run_workflow([transport], [ClientPlan(info_value=self.info())])
        self.assertTrue(result.ok)
        self.assertEqual(result.verification.classification, VerificationPhaseClassification.MATCH)
        self.assertEqual(result.verification.reconnect_attempts, 1)
        self.assertEqual(len(flashes), 1)
        self.assertTrue(transport.closed)
        self.assertTrue(clients[0].closed)
        self.assertEqual(self.last_client_parameters, [(1.0, 2)])

    def test_first_open_failure_reconnects_without_reflash(self) -> None:
        failed = FakeTransport(open_error=TransportError("not ready"), clock=self.clock)
        ready = self.ready_transport()
        result, flashes, clients = self.run_workflow(
            [failed, ready], [ClientPlan(info_value=self.info())]
        )
        self.assertTrue(result.ok)
        self.assertEqual(result.verification.reconnect_attempts, 2)
        self.assertEqual(len(flashes), 1)
        self.assertTrue(failed.closed)
        self.assertTrue(ready.closed)
        self.assertEqual(len(clients), 1)

    def test_hello_timeout_and_session_loss_retry_with_fresh_clients(self) -> None:
        result, flashes, clients = self.run_workflow(
            [self.ready_transport(), self.ready_transport(), self.ready_transport()],
            [
                ClientPlan(connect_error=RequestTimeoutError("timeout", request_id=0, attempts=2)),
                ClientPlan(connect_error=SessionLostError("reset", request_id=0)),
                ClientPlan(info_value=self.info()),
            ],
        )
        self.assertTrue(result.ok)
        self.assertEqual(result.verification.reconnect_attempts, 3)
        self.assertEqual(len(flashes), 1)
        self.assertEqual(len(clients), 3)
        self.assertTrue(all(client.closed for client in clients))

    def test_mismatch_and_identity_unavailable_are_definitive_without_reflash(self) -> None:
        mismatch, flashes, _ = self.run_workflow(
            [self.ready_transport()], [ClientPlan(info_value=self.info(target="esp32c6"))]
        )
        self.assertEqual(mismatch.verification.classification, VerificationPhaseClassification.MISMATCH)
        self.assertEqual(len(flashes), 1)

        unavailable_info = {
            "project": "s3-hidbot",
            "target": "esp32s3",
            "idf_version": "v5.5.4",
            "protocol_version": 1,
            "firmware": {
                "version": "0.1.0-dev",
                "source_revision": None,
                "app_elf_sha256": "b" * 64,
                "build_profile": "freenove-fnk0085",
            },
        }
        unavailable, flashes, _ = self.run_workflow(
            [self.ready_transport()], [ClientPlan(info_value=unavailable_info)]
        )
        self.assertEqual(
            unavailable.verification.classification,
            VerificationPhaseClassification.IDENTITY_UNAVAILABLE,
        )
        self.assertEqual(len(flashes), 1)

    def test_transport_exhaustion_and_startup_timeout_are_distinct(self) -> None:
        transports = [
            FakeTransport(open_error=TransportError("unavailable"), clock=self.clock)
            for _ in range(4)
        ]
        unavailable, flashes, _ = self.run_workflow(transports, [])
        self.assertEqual(
            unavailable.verification.classification,
            VerificationPhaseClassification.TRANSPORT_UNAVAILABLE,
        )
        self.assertEqual(len(flashes), 1)

        timeout, flashes, _ = self.run_workflow(
            [self.ready_transport() for _ in range(4)],
            [
                ClientPlan(connect_error=RequestTimeoutError("timeout", request_id=0, attempts=2))
                for _ in range(4)
            ],
        )
        self.assertEqual(timeout.verification.classification, VerificationPhaseClassification.TIMEOUT)
        self.assertEqual(len(flashes), 1)

    def test_protocol_compatibility_and_remote_errors_are_definitive(self) -> None:
        cases = (
            (ProtocolError("bad"), VerificationPhaseClassification.PROTOCOL_ERROR),
            (CompatibilityError("bad"), VerificationPhaseClassification.COMPATIBILITY_ERROR),
            (
                RemoteError("UNKNOWN_COMMAND", "bad", request_id=0, session=TOKEN),
                VerificationPhaseClassification.REMOTE_ERROR,
            ),
        )
        for error, expected in cases:
            with self.subTest(expected=expected):
                result, flashes, clients = self.run_workflow(
                    [self.ready_transport()], [ClientPlan(info_error=error)]
                )
                self.assertEqual(result.verification.classification, expected)
                self.assertEqual(len(flashes), 1)
                self.assertTrue(clients[0].closed)

    def test_flash_failure_never_constructs_uart(self) -> None:
        with self.assertRaises(FlashExecutionError):
            self.run_workflow(
                [],
                [],
                flash_error=FlashExecutionError("flash failed", attempts=3),
            )
        self.assertEqual(len(self.last_flash_calls), 1)
        self.assertEqual(self.last_transport_factory_calls, [])
        self.assertEqual(self.last_client_parameters, [])

    def test_keyboard_interrupt_propagates_without_reflash_and_closes(self) -> None:
        transport = self.ready_transport()
        with self.assertRaises(KeyboardInterrupt):
            self.run_workflow(
                [transport], [ClientPlan(connect_error=KeyboardInterrupt())]
            )
        self.assertTrue(transport.closed)
        self.assertEqual(len(self.last_flash_calls), 1)

    def test_reset_noise_drain_reaches_quiet_without_framing(self) -> None:
        streams = (
            (),
            (b"\x00\x00\x00\n",),
            (b"\x00" * 600,),
            (b"I (1) boot\r\n",),
            (b"@@HIDBOT", b" fragment",),
            (b"@HID",),
            (b"boot-without-newline",),
            (b'@HIDBOT {"stale":true}\n',),
        )
        for chunks in streams:
            with self.subTest(chunks=chunks):
                self.clock.value = 0.0
                transport = self.ready_transport(*chunks)
                self.assertTrue(
                    _drain_to_quiet(
                        transport,
                        clock=self.clock,
                        sleeper=self.clock.sleep,
                        overall_deadline=20.0,
                    )
                )

    def test_drain_handles_chunk_boundaries_and_enforces_duration_and_byte_caps(self) -> None:
        self.clock.value = 0.0
        split = self.ready_transport(b"@", b"@H", b"ID", b"BOT", b" noise")
        self.assertTrue(
            _drain_to_quiet(split, clock=self.clock, sleeper=self.clock.sleep, overall_deadline=20.0)
        )

        self.clock.value = 0.0
        continuous = FakeTransport(continuous=b"x", read_advance=0.1, clock=self.clock)
        self.assertFalse(
            _drain_to_quiet(
                continuous, clock=self.clock, sleeper=self.clock.sleep, overall_deadline=20.0
            )
        )

        self.clock.value = 0.0
        bytes_over = FakeTransport(continuous=b"x" * 512, clock=self.clock)
        self.assertFalse(
            _drain_to_quiet(
                bytes_over, clock=self.clock, sleeper=self.clock.sleep, overall_deadline=20.0
            )
        )
        self.assertEqual(bytes_over.read_calls, MAX_DRAIN_BYTES // 512)

    def test_first_reconnect_noise_then_clean_match(self) -> None:
        noisy = FakeTransport(continuous=b"x", read_advance=0.1, clock=self.clock)
        result, flashes, clients = self.run_workflow(
            [noisy, self.ready_transport()], [ClientPlan(info_value=self.info())]
        )
        self.assertTrue(result.ok)
        self.assertEqual(result.verification.reconnect_attempts, 2)
        self.assertEqual(len(flashes), 1)
        self.assertEqual(len(clients), 1)


if __name__ == "__main__":
    unittest.main()
