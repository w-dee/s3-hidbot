from __future__ import annotations

import json
import unittest
from collections import deque

from hidbot.client import Client
from hidbot.errors import (
    CompatibilityError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
)
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC
from hidbot.protocol import (
    BASELINE_REQUIRED_CAPABILITIES,
    MAX_ID,
    OutputRoute,
    OutputRouteV2,
)


TOKEN = "0123456789abcdef0123456789abcdef"
BOOT_ID = "abcdef0123456789abcdef0123456789"
NONCE = "fedcba9876543210fedcba9876543210"
OTHER_NONCE = "00112233445566778899aabbccddeeff"


def response(
    request_id: int,
    session: str | None,
    *,
    result: object | None = None,
    error: dict[str, str] | None = None,
    ok: bool | None = None,
) -> bytes:
    if ok is None:
        ok = error is None
    value: dict[str, object] = {
        "type": "response",
        "v": 1,
        "id": request_id,
        "session": session,
        "ok": ok,
    }
    if error is None:
        value["result"] = result if result is not None else {}
    else:
        value["error"] = error
    return FRAME_PREFIX + json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"


def hello_response(
    request_id: int,
    nonce: str,
    session: str = TOKEN,
    capabilities: list[str] | None = None,
) -> bytes:
    if capabilities is None:
        capabilities = [
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
            "ble.exposure-control-v1",
        ]
    return response(
        request_id,
        session,
        result={
            "project": "s3-hidbot",
            "protocol_version": 1,
            "client_nonce": nonce,
            "boot_id": BOOT_ID,
            "session": session,
            "lease_ms": 5000,
            "capabilities": capabilities,
        },
    )


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def now(self) -> float:
        return self.value

    def sleep(self, duration: float) -> None:
        self.value += duration


class FakeTransport:
    def __init__(self, on_write=None) -> None:
        self.writes: list[bytes] = []
        self.chunks: deque[bytes] = deque()
        self.on_write = on_write
        self.closed = False

    def write(self, data: bytes) -> None:
        self.writes.append(data)
        if self.on_write is not None:
            self.on_write(self, data)

    def read(self, max_bytes: int, timeout: float) -> bytes:
        del max_bytes, timeout
        return self.chunks.popleft() if self.chunks else b""

    def close(self) -> None:
        self.closed = True


def request_object(frame: bytes) -> dict[str, object]:
    assert frame.startswith(FRAME_PREFIX)
    return json.loads(frame[len(FRAME_PREFIX) : -1])


class ClientTests(unittest.TestCase):
    def make_client(self, transport: FakeTransport, *, max_attempts: int = 3) -> Client:
        clock = FakeClock()
        return Client(
            transport,
            timeout=0.03,
            max_attempts=max_attempts,
            clock=clock.now,
            sleeper=clock.sleep,
            nonce_factory=lambda: NONCE,
        )

    def test_connect_and_diagnostic_commands(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            request_value = request_object(data)
            if request_value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(request_value["id"], request_value["params"]["client_nonce"]))
            elif request_value["cmd"] == "system.ping":
                transport.chunks.append(response(request_value["id"], TOKEN, result={"pong": True}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        hello = client.connect()
        self.assertEqual(hello.session, TOKEN)
        self.assertEqual(hello.lease_ms, 5000)
        self.assertEqual(client.lease_ms, 5000)
        self.assertEqual(client.ping(), {"pong": True})
        self.assertEqual(transport.writes[0], TRANSPORT_SYNC)

    def test_release_all_returns_typed_result_and_canonical_params(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            request_value = request_object(data)
            if request_value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(request_value["id"], request_value["params"]["client_nonce"]))
            elif request_value["cmd"] == "hid.release_all":
                self.assertEqual(request_value["params"], {})
                transport.chunks.append(
                    response(
                        request_value["id"],
                        TOKEN,
                        result={"keyboard": "already_up", "mouse": "submitted"},
                    )
                )

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        result = client.release_all()
        self.assertEqual(result.keyboard, "already_up")
        self.assertEqual(result.mouse, "submitted")

    def test_usb_exposure_primitives_require_capability_and_retire_local_session(self) -> None:
        status = {
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

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            request_value = request_object(data)
            if request_value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(request_value["id"], request_value["params"]["client_nonce"])
                )
            elif request_value["cmd"] in {"usb.exposure.status", "usb.attach", "usb.detach"}:
                self.assertEqual(request_value["params"], {})
                transport.chunks.append(response(request_value["id"], TOKEN, result=status))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        self.assertEqual(client.usb_exposure_status().observed, "attaching")
        attached = client.usb_attach()
        self.assertEqual(attached.generation, 1)
        self.assertIsNone(client.session)
        with self.assertRaises(SessionLostError):
            client.ping()
        writes_before = len(transport.writes)
        client.connect()
        self.assertEqual(client.usb_detach().desired, "exposed")
        self.assertIsNone(client.session)
        self.assertGreater(len(transport.writes), writes_before)

    def test_usb_exposure_missing_capability_is_zero_wire(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        client._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        for method in (client.usb_exposure_status, client.usb_attach, client.usb_detach):
            with self.assertRaises(CompatibilityError):
                method()
        self.assertEqual(client.session, TOKEN)
        self.assertEqual(transport.writes, [])

    def test_ble_exposure_is_typed_capability_gated_and_keeps_session(self) -> None:
        status = {
            "desired": "exposed",
            "observed": "enabling",
            "generation": 1,
            "stack_ready": False,
            "advertising": False,
            "connected": False,
            "recovery_required": False,
            "last_error": None,
        }

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(value["id"], value["params"]["client_nonce"])
                )
            elif value["cmd"] in {"ble.exposure.status", "ble.enable", "ble.disable"}:
                self.assertEqual(value["params"], {})
                transport.chunks.append(response(value["id"], TOKEN, result=status))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        self.assertEqual(client.ble_exposure_status().generation, 1)
        self.assertEqual(client.ble_enable().observed.value, "enabling")
        self.assertEqual(client.ble_disable().desired.value, "exposed")
        self.assertEqual(client.session, TOKEN)

        missing_transport = FakeTransport()
        missing = self.make_client(missing_transport)
        missing._session = TOKEN
        missing._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        for method in (missing.ble_exposure_status, missing.ble_enable, missing.ble_disable):
            with self.assertRaises(CompatibilityError):
                method()
        self.assertEqual(missing_transport.writes, [])

    def test_hid_route_primitives_are_typed_strict_and_capability_gated(self) -> None:
        route_status = {
            "desired": "usb",
            "active": "usb",
            "generation": 7,
            "transition": "stable",
            "ready": True,
        }

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(value["id"], value["params"]["client_nonce"])
                )
            elif value["cmd"] == "hid.route.status":
                self.assertEqual(value["params"], {})
                transport.chunks.append(response(value["id"], TOKEN, result=route_status))
            elif value["cmd"] == "hid.route.set":
                self.assertEqual(value["params"], {"route": "usb"})
                transport.chunks.append(response(value["id"], TOKEN, result=route_status))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        self.assertEqual(client.hid_route_status().active, OutputRoute.USB)
        self.assertTrue(client.hid_route_status().ready)
        self.assertEqual(client.hid_route_set(OutputRoute.USB).generation, 7)
        self.assertIsNone(client.session)

        missing = self.make_client(FakeTransport())
        missing._session = TOKEN
        missing._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        with self.assertRaises(CompatibilityError):
            missing.hid_route_status()
        with self.assertRaises(CompatibilityError):
            missing.hid_route_set(OutputRoute.USB)
        with self.assertRaises(ProtocolError):
            missing.hid_route_set("ble")
        self.assertEqual(missing._transport.writes, [])

    def test_hid_route_v2_is_preferred_and_v1_fallback_rejects_ble_locally(self) -> None:
        route_status = {
            "desired": "ble",
            "active": "ble",
            "generation": 9,
            "transition": "stable",
            "ready": True,
        }

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(
                        value["id"],
                        value["params"]["client_nonce"],
                        capabilities=[
                            *sorted(BASELINE_REQUIRED_CAPABILITIES),
                            "hid.output-route-v1",
                            "hid.output-route-v2",
                        ],
                    )
                )
            elif value["cmd"] == "hid.route.v2.status":
                transport.chunks.append(response(value["id"], TOKEN, result=route_status))
            elif value["cmd"] == "hid.route.v2.set":
                self.assertEqual(value["params"], {"route": "ble"})
                transport.chunks.append(response(value["id"], TOKEN, result=route_status))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        self.assertEqual(client.hid_route_protocol_version, 2)
        self.assertEqual(client.hid_route_status().active, OutputRouteV2.BLE)
        self.assertEqual(client.hid_route_set("ble").desired, OutputRouteV2.BLE)
        commands = [request_object(frame)["cmd"] for frame in transport.writes
                    if frame != TRANSPORT_SYNC]
        self.assertEqual(
            commands,
            ["protocol.hello", "hid.route.v2.status", "hid.route.v2.set"],
        )

        fallback_transport = FakeTransport()
        fallback = self.make_client(fallback_transport)
        fallback._session = TOKEN
        fallback._capabilities = ("hid.output-route-v1",)
        self.assertEqual(fallback.hid_route_protocol_version, 1)
        with self.assertRaises(CompatibilityError):
            fallback.hid_route_set("ble")
        self.assertEqual(fallback_transport.writes, [])

    def test_keyboard_report_returns_typed_result_and_canonical_params(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            request_value = request_object(data)
            if request_value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(request_value["id"], request_value["params"]["client_nonce"]))
            elif request_value["cmd"] == "hid.keyboard.report":
                self.assertEqual(
                    list(request_value["params"]), ["modifiers", "keys"]
                )
                self.assertEqual(request_value["params"], {"modifiers": 2, "keys": [4, 5]})
                transport.chunks.append(response(request_value["id"], TOKEN, result={"state": "submitted"}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        result = client.keyboard_report(2, (4, 5))
        self.assertEqual(result.state, "submitted")

    def test_keyboard_report_rejects_invalid_input_before_transport_write(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        for modifiers, keys in ((True, []), (0, [5, 4]), (0, [0]), (0, [0xE0])):
            with self.assertRaises(ProtocolError):
                client.keyboard_report(modifiers, keys)
        self.assertEqual(transport.writes, [])

    def test_keyboard_report_missing_capability_is_zero_wire_and_preserves_state(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        client._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        before_id = client._next_request_id
        with self.assertRaises(CompatibilityError):
            client.keyboard_report(0, [4])
        self.assertEqual(client.session, TOKEN)
        self.assertEqual(client._next_request_id, before_id)
        self.assertEqual(transport.writes, [])

    def test_missing_capability_still_validates_input_first(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        client._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        with self.assertRaises(ProtocolError):
            client.keyboard_report(999, [4])
        with self.assertRaises(ProtocolError):
            client.mouse_report(0, -128, 0, 0, 0)
        self.assertEqual(client._next_request_id, 0)
        self.assertEqual(transport.writes, [])

    def test_mouse_report_returns_typed_result_and_canonical_params(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            request_value = request_object(data)
            if request_value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(request_value["id"], request_value["params"]["client_nonce"])
                )
            elif request_value["cmd"] == "hid.mouse.report":
                self.assertEqual(
                    list(request_value["params"]), ["buttons", "x", "y", "wheel", "pan"]
                )
                self.assertEqual(
                    request_value["params"],
                    {"buttons": 3, "x": 1, "y": -2, "wheel": 0, "pan": 4},
                )
                transport.chunks.append(
                    response(request_value["id"], TOKEN, result={"state": "submitted"})
                )

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        result = client.mouse_report(3, 1, -2, 0, 4)
        self.assertEqual(result.state, "submitted")

    def test_mouse_report_rejects_invalid_input_before_transport_write(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        for values in (
            (True, 0, 0, 0, 0),
            (32, 0, 0, 0, 0),
            (0, -128, 0, 0, 0),
            (0, 128, 0, 0, 0),
            (0, 0.5, 0, 0, 0),
            (0, 0, 0, False, 0),
        ):
            with self.assertRaises(ProtocolError):
                client.mouse_report(*values)
        self.assertEqual(transport.writes, [])

    def test_mouse_report_missing_capability_is_zero_wire_and_preserves_state(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        client._session = TOKEN
        client._capabilities = tuple(BASELINE_REQUIRED_CAPABILITIES)
        before_id = client._next_request_id
        with self.assertRaises(CompatibilityError):
            client.mouse_report(0, 1, 0, 0, 0)
        self.assertEqual(client.session, TOKEN)
        self.assertEqual(client._next_request_id, before_id)
        self.assertEqual(transport.writes, [])

    def test_connect_missing_baseline_capability_fails_as_compatibility_error(self) -> None:
        missing = sorted(BASELINE_REQUIRED_CAPABILITIES - {"hid.release-all-v1"})

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(
                    hello_response(
                        value["id"],
                        value["params"]["client_nonce"],
                        capabilities=missing,
                    )
                )

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        with self.assertRaises(CompatibilityError):
            client.connect()
        self.assertIsNone(client.session)
        self.assertEqual(
            len([item for item in transport.writes if item.startswith(FRAME_PREFIX)]),
            1,
        )

    def test_hello_retry_is_byte_identical_and_nonce_is_stable(self) -> None:
        hello_writes = 0

        def on_write(transport: FakeTransport, data: bytes) -> None:
            nonlocal hello_writes
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            hello_writes += 1
            if hello_writes == 2:
                transport.chunks.append(hello_response(value["id"], value["params"]["client_nonce"]))

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=2)
        client.connect()
        self.assertEqual(hello_writes, 2)
        self.assertEqual(transport.writes[1], transport.writes[2])

    def test_stale_nonce_before_correct_hello_is_ignored(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            transport.chunks.append(hello_response(value["id"], OTHER_NONCE, session=TOKEN))
            transport.chunks.append(hello_response(value["id"], NONCE, session=TOKEN))

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=1)
        self.assertEqual(client.connect().client_nonce, NONCE)

    def test_normal_retry_is_byte_identical(self) -> None:
        ping_writes = 0

        def on_write(transport: FakeTransport, data: bytes) -> None:
            nonlocal ping_writes
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(value["id"], NONCE))
            else:
                ping_writes += 1
                if ping_writes == 2:
                    transport.chunks.append(response(value["id"], TOKEN, result={"pong": True}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=2)
        client.connect()
        self.assertEqual(client.ping(), {"pong": True})
        ping_frames = [item for item in transport.writes if item.startswith(FRAME_PREFIX) and b"system.ping" in item]
        self.assertEqual(ping_frames, [ping_frames[0], ping_frames[0]])

    def test_wrong_id_and_session_null_diagnostic_do_not_complete(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(value["id"], NONCE))
            else:
                transport.chunks.append(response(value["id"] + 1, TOKEN, result={"pong": True}))
                transport.chunks.append(response(value["id"], None, error={"code": "MALFORMED_JSON", "message": "bad"}))
                transport.chunks.append(response(value["id"], TOKEN, result={"pong": True}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=1)
        client.connect()
        self.assertEqual(client.ping(), {"pong": True})

    def test_session_mismatch_stops_retry_and_never_replays(self) -> None:
        ping_writes = 0

        def on_write(transport: FakeTransport, data: bytes) -> None:
            nonlocal ping_writes
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(value["id"], NONCE))
            else:
                ping_writes += 1
                transport.chunks.append(response(value["id"], None, error={"code": "SESSION_MISMATCH", "message": "stale"}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=3)
        client.connect()
        with self.assertRaises(SessionLostError):
            client.ping()
        self.assertEqual(ping_writes, 1)

    def test_unmatched_limit_and_correlated_remote_error(self) -> None:
        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(value["id"], NONCE))
            else:
                transport.chunks.extend(
                    response(value["id"] + index + 1, TOKEN, result={}) for index in range(33)
                )

        transport = FakeTransport(on_write)
        client = self.make_client(transport, max_attempts=1)
        client.connect()
        with self.assertRaises(ProtocolError):
            client.ping()

        def on_error(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                transport.chunks.append(hello_response(value["id"], NONCE))
            else:
                transport.chunks.append(response(value["id"], TOKEN, error={"code": "UNKNOWN_COMMAND", "message": "x"}))

        error_transport = FakeTransport(on_error)
        error_client = self.make_client(error_transport, max_attempts=1)
        error_client.connect()
        with self.assertRaisesRegex(RemoteError, "UNKNOWN_COMMAND"):
            error_client.ping()

    def test_timeout_then_fresh_hello_does_not_replay_old_command(self) -> None:
        nonces = iter((NONCE, OTHER_NONCE))

        def on_write(transport: FakeTransport, data: bytes) -> None:
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                nonce = value["params"]["client_nonce"]
                session = TOKEN if nonce == NONCE else "a" * 32
                transport.chunks.append(hello_response(value["id"], nonce, session=session))

        transport = FakeTransport(on_write)
        clock = FakeClock()
        client = Client(
            transport,
            timeout=0.02,
            max_attempts=1,
            clock=clock.now,
            sleeper=clock.sleep,
            nonce_factory=lambda: next(nonces),
        )
        client.connect()
        with self.assertRaises(RequestTimeoutError):
            client.ping()
        old_ping = [item for item in transport.writes if b"system.ping" in item][0]
        client.connect()
        self.assertEqual([item for item in transport.writes if item == old_ping], [old_ping])

    def test_close_invalidates_state_without_writing(self) -> None:
        transport = FakeTransport()
        client = self.make_client(transport)
        before = len(transport.writes)
        client.close()
        self.assertTrue(transport.closed)
        self.assertEqual(len(transport.writes), before)
        with self.assertRaises(Exception):
            client.ping()

    def test_normal_id_exhaustion_starts_a_fresh_session(self) -> None:
        hello_count = 0

        def on_write(transport: FakeTransport, data: bytes) -> None:
            nonlocal hello_count
            if data == TRANSPORT_SYNC:
                return
            value = request_object(data)
            if value["cmd"] == "protocol.hello":
                hello_count += 1
                nonce = value["params"]["client_nonce"]
                session = TOKEN if hello_count == 1 else "a" * 32
                transport.chunks.append(hello_response(value["id"], nonce, session=session))
            else:
                transport.chunks.append(response(value["id"], "a" * 32, result={"pong": True}))

        transport = FakeTransport(on_write)
        client = self.make_client(transport)
        client.connect()
        client._next_request_id = MAX_ID + 1
        self.assertEqual(client.ping(), {"pong": True})
        self.assertEqual(hello_count, 2)
        self.assertEqual(request_object([item for item in transport.writes if b"system.ping" in item][-1])["id"], 0)


if __name__ == "__main__":
    unittest.main()
