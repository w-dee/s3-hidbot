from __future__ import annotations

import json
import unittest
from collections import deque

from hidbot.client import Client
from hidbot.errors import ProtocolError, RemoteError, RequestTimeoutError, SessionLostError
from hidbot.framing import FRAME_PREFIX, TRANSPORT_SYNC
from hidbot.protocol import MAX_ID


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


def hello_response(request_id: int, nonce: str, session: str = TOKEN) -> bytes:
    return response(
        request_id,
        session,
        result={
            "project": "s3-hidbot",
            "protocol_version": 1,
            "client_nonce": nonce,
            "boot_id": BOOT_ID,
            "session": session,
            "capabilities": [
                "protocol.hello-v1",
                "system.ping-v1",
                "system.info-v1",
                "usb.status-v1",
            ],
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
        self.assertEqual(client.ping(), {"pong": True})
        self.assertEqual(transport.writes[0], TRANSPORT_SYNC)

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
