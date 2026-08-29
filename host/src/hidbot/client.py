"""Stop-and-wait U3.2 client core over a generic bounded byte transport."""

from __future__ import annotations

import secrets
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol

from .errors import (
    CompatibilityError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .framing import Framer, MachineFrame, MachineFrameIssue, TRANSPORT_SYNC
from .protocol import (
    MAX_ID,
    Response,
    build_command_frame,
    build_hello_frame,
    parse_response,
    validate_hello_response,
)


class ByteTransport(Protocol):
    """The only transport surface the pure client core requires."""

    def write(self, data: bytes) -> None: ...

    def read(self, max_bytes: int, timeout: float) -> bytes: ...

    def close(self) -> None: ...


Clock = Callable[[], float]
Sleeper = Callable[[float], None]
NonceFactory = Callable[[], str]
LogSink = Callable[[bytes], None]


@dataclass(frozen=True)
class HelloResult:
    session: str
    boot_id: str
    client_nonce: str
    capabilities: tuple[str, ...]
    lease_ms: int


class Client:
    """A single-owner, one-outstanding-request v1 client.

    Automatic retries are limited to the same session, ID, and immutable frame
    bytes.  A fresh hello never replays an in-flight command from an old
    session.
    """

    def __init__(
        self,
        transport: ByteTransport,
        *,
        timeout: float = 1.0,
        max_attempts: int = 3,
        unmatched_limit: int = 32,
        clock: Clock = time.monotonic,
        sleeper: Sleeper = time.sleep,
        nonce_factory: NonceFactory | None = None,
        log_sink: LogSink | None = None,
    ) -> None:
        if timeout <= 0 or max_attempts < 1 or unmatched_limit < 1:
            raise ValueError("timeout, max_attempts, and unmatched_limit must be positive")
        self._transport = transport
        self._timeout = timeout
        self._max_attempts = max_attempts
        self._unmatched_limit = unmatched_limit
        self._clock = clock
        self._sleeper = sleeper
        self._nonce_factory = nonce_factory or (lambda: secrets.token_hex(16))
        self._framer = Framer(log_sink=log_sink)
        self._lock = threading.Lock()
        self._closed = False
        self._session: str | None = None
        self._boot_id: str | None = None
        self._capabilities: tuple[str, ...] = ()
        self._lease_ms: int | None = None
        self._next_hello_id = 0
        self._next_request_id = 0

    @property
    def session(self) -> str | None:
        return self._session

    @property
    def boot_id(self) -> str | None:
        return self._boot_id

    @property
    def capabilities(self) -> tuple[str, ...]:
        return self._capabilities

    @property
    def lease_ms(self) -> int | None:
        return self._lease_ms

    def _ensure_open(self) -> None:
        if self._closed:
            raise TransportError("client is closed")

    def _invalidate_session(self) -> None:
        self._session = None
        self._boot_id = None
        self._capabilities = ()
        self._lease_ms = None

    def _write(self, data: bytes) -> None:
        try:
            self._transport.write(data)
        except Exception as exc:  # transport implementations may use varied exceptions
            raise TransportError("transport write failed") from exc

    def _read(self, timeout: float) -> bytes:
        try:
            data = self._transport.read(512, timeout)
        except Exception as exc:  # transport implementations may use varied exceptions
            raise TransportError("transport read failed") from exc
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TransportError("transport returned a non-byte value")
        return bytes(data)

    def _wait_for_response(
        self,
        *,
        expected_id: int,
        expected_session: str | None,
        hello: bool,
        expected_nonce: str | None = None,
        unmatched_count: int = 0,
    ) -> tuple[Response | None, tuple[str, ...], int]:
        deadline = self._clock() + self._timeout
        diagnostics: list[str] = []

        def record_unmatched() -> None:
            nonlocal unmatched_count
            unmatched_count += 1
            if unmatched_count > self._unmatched_limit:
                raise ProtocolError("unmatched machine-frame limit exceeded")

        while True:
            remaining = deadline - self._clock()
            if remaining <= 0:
                return None, tuple(diagnostics), unmatched_count
            chunk = self._read(min(0.05, remaining))
            if not chunk:
                self._sleeper(min(0.01, remaining))
                continue
            for event in self._framer.feed(chunk):
                if isinstance(event, MachineFrameIssue):
                    record_unmatched()
                    continue
                assert isinstance(event, MachineFrame)
                try:
                    response = parse_response(event.payload)
                except ProtocolError:
                    record_unmatched()
                    continue
                if response.response_id != expected_id:
                    record_unmatched()
                    continue
                if hello:
                    if response.ok and response.session is not None:
                        if (
                            expected_nonce is not None
                            and isinstance(response.result, dict)
                            and response.result.get("client_nonce") != expected_nonce
                        ):
                            if len(diagnostics) < 8:
                                diagnostics.append("STALE_HELLO_NONCE")
                            record_unmatched()
                            continue
                        return response, tuple(diagnostics), unmatched_count
                    if response.error is not None and response.session is None:
                        if len(diagnostics) < 8:
                            diagnostics.append(response.error.code)
                        if response.error.code == "SESSION_MISMATCH":
                            self._invalidate_session()
                            raise SessionLostError(
                                "device rejected the hello due to session loss",
                                request_id=expected_id,
                            )
                        record_unmatched()
                    else:
                        record_unmatched()
                else:
                    if response.session == expected_session:
                        return response, tuple(diagnostics), unmatched_count
                    if response.session is None and response.error is not None:
                        if len(diagnostics) < 8:
                            diagnostics.append(response.error.code)
                        if response.error.code == "SESSION_MISMATCH":
                            self._invalidate_session()
                            raise SessionLostError(
                                "device rejected the request due to session loss",
                                request_id=expected_id,
                            )
                    record_unmatched()

    def _hello_locked(self) -> HelloResult:
        self._ensure_open()
        if self._next_hello_id > MAX_ID:
            raise SessionLostError("hello request ID space exhausted")
        hello_id = self._next_hello_id
        self._next_hello_id += 1
        nonce = self._nonce_factory()
        frame = build_hello_frame(hello_id, nonce)
        self._framer.reset()
        self._write(TRANSPORT_SYNC)
        diagnostics: tuple[str, ...] = ()
        unmatched_count = 0
        for _attempt in range(1, self._max_attempts + 1):
            self._write(frame)
            response, attempt_diagnostics, unmatched_count = self._wait_for_response(
                expected_id=hello_id,
                expected_session=None,
                hello=True,
                expected_nonce=nonce,
                unmatched_count=unmatched_count,
            )
            diagnostics = attempt_diagnostics or diagnostics
            if response is None:
                continue
            try:
                hello = validate_hello_response(
                    response,
                    expected_id=hello_id,
                    expected_nonce=nonce,
                )
            except ProtocolError as exc:
                # A different nonce is a stale hello; do not let it establish
                # a session. Other schema-valid identity failures are a peer
                # compatibility error and must not be retried indefinitely.
                if (
                    isinstance(response.result, dict)
                    and response.result.get("client_nonce") != nonce
                ):
                    diagnostics = diagnostics + ("STALE_HELLO_NONCE",)
                    continue
                raise CompatibilityError(str(exc)) from exc
            self._session = hello.session
            self._boot_id = hello.boot_id
            self._capabilities = hello.capabilities
            self._lease_ms = hello.lease_ms
            self._next_request_id = 0
            return HelloResult(
                hello.session,
                hello.boot_id,
                hello.client_nonce,
                hello.capabilities,
                hello.lease_ms,
            )
        raise RequestTimeoutError(
            "timed out waiting for a correlated protocol.hello response",
            request_id=hello_id,
            attempts=self._max_attempts,
            diagnostics=diagnostics,
        )

    def connect(self) -> HelloResult:
        """Start a fresh session; no old command is replayed."""

        with self._lock:
            self._invalidate_session()
            return self._hello_locked()

    def _request_locked(self, command: str) -> object:
        self._ensure_open()
        if self._session is None:
            raise SessionLostError("client has no active session")
        if self._next_request_id > MAX_ID:
            self._invalidate_session()
            self._hello_locked()
        assert self._session is not None
        request_id = self._next_request_id
        self._next_request_id += 1
        # This frame is immutable for every retry attempt.
        frame = build_command_frame(request_id, self._session, command)
        diagnostics: tuple[str, ...] = ()
        unmatched_count = 0
        for _attempt in range(1, self._max_attempts + 1):
            self._write(frame)
            response, attempt_diagnostics, unmatched_count = self._wait_for_response(
                expected_id=request_id,
                expected_session=self._session,
                hello=False,
                unmatched_count=unmatched_count,
            )
            diagnostics = attempt_diagnostics or diagnostics
            if response is None:
                continue
            if response.error is not None:
                assert response.session == self._session
                raise RemoteError(
                    response.error.code,
                    response.error.message,
                    request_id=request_id,
                    session=self._session,
                )
            if not response.ok:
                raise ProtocolError("correlated response has an invalid ok/result shape")
            return response.result
        raise RequestTimeoutError(
            f"timed out waiting for response id {request_id}",
            request_id=request_id,
            attempts=self._max_attempts,
            diagnostics=diagnostics,
        )

    def ping(self) -> object:
        with self._lock:
            return self._request_locked("system.ping")

    def info(self) -> object:
        with self._lock:
            return self._request_locked("system.info")

    def usb_status(self) -> object:
        with self._lock:
            return self._request_locked("usb.status")

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            try:
                self._transport.close()
            except Exception as exc:
                raise TransportError("transport close failed") from exc
            finally:
                self._closed = True
                self._invalidate_session()
