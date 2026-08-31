"""Bounded post-flash runtime identity verification orchestration.

This module composes the existing programming, transport, protocol, and
identity-comparison authorities without changing their individual contracts.
It is deliberately the only layer that connects a successful programming
result to a fresh post-reset UART session.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum
from typing import Protocol

from .client import Client, HelloResult
from .errors import (
    CompatibilityError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .firmware_verification import (
    FirmwareVerificationClassification,
    FirmwareVerificationResult,
    compare_firmware_identity,
)
from .flashing import FlashExecutionResult, execute_flash
from .protocol import validate_system_info
from .provisioning import VerifiedFirmwareBundle
from .serial_transport import PySerialTransport


CONTROL_UART_BAUD = 115200
READINESS_DEADLINE_SECONDS = 20.0
MAX_CONNECTION_ATTEMPTS = 4
MAX_DRAIN_SECONDS = 0.5
RX_QUIET_SECONDS = 0.1
MAX_DRAIN_BYTES = 8192
RECONNECT_INTERVAL_SECONDS = 0.25
REQUEST_TIMEOUT_SECONDS = 1.0
CLIENT_MAX_ATTEMPTS = 2
READ_CHUNK_BYTES = 512
READ_POLL_SECONDS = 0.05
_CLOCK_EPSILON_SECONDS = 1e-9


class VerificationPhaseClassification(str, Enum):
    """Terminal post-programming verification states."""

    MATCH = "MATCH"
    MISMATCH = "MISMATCH"
    IDENTITY_UNAVAILABLE = "IDENTITY_UNAVAILABLE"
    TRANSPORT_UNAVAILABLE = "TRANSPORT_UNAVAILABLE"
    TIMEOUT = "TIMEOUT"
    PROTOCOL_ERROR = "PROTOCOL_ERROR"
    COMPATIBILITY_ERROR = "COMPATIBILITY_ERROR"
    REMOTE_ERROR = "REMOTE_ERROR"


class FlashPhaseClassification(str, Enum):
    """Programming-plane state retained after successful esptool execution."""

    FLASHED = "FLASHED"


class ProvisioningWorkflowClassification(str, Enum):
    """Top-level result retaining the completed programming phase."""

    FLASHED_AND_VERIFIED = "FLASHED_AND_VERIFIED"
    FLASHED_VERIFICATION_FAILED = "FLASHED_VERIFICATION_FAILED"


@dataclass(frozen=True, slots=True)
class VerificationPhaseResult:
    """One bounded post-flash verification result without raw UART data."""

    classification: VerificationPhaseClassification
    reconnect_attempts: int
    boot_id: str | None = None
    firmware_verification: FirmwareVerificationResult | None = None

    @property
    def match(self) -> bool:
        return self.classification is VerificationPhaseClassification.MATCH


@dataclass(frozen=True, slots=True)
class ProvisioningWorkflowResult:
    """Programming and runtime-verification phases for one CLI invocation."""

    flash: FlashExecutionResult
    verification: VerificationPhaseResult
    flash_classification: FlashPhaseClassification = FlashPhaseClassification.FLASHED

    @property
    def classification(self) -> ProvisioningWorkflowClassification:
        if self.verification.match:
            return ProvisioningWorkflowClassification.FLASHED_AND_VERIFIED
        return ProvisioningWorkflowClassification.FLASHED_VERIFICATION_FAILED

    @property
    def ok(self) -> bool:
        return self.verification.match


class _Transport(Protocol):
    def open(self) -> None: ...

    def read(self, max_bytes: int, timeout: float) -> bytes: ...

    def close(self) -> None: ...


class _Client(Protocol):
    def connect(self) -> HelloResult: ...

    def info(self) -> object: ...

    def close(self) -> None: ...


Clock = Callable[[], float]
Sleeper = Callable[[float], None]
FlashExecutor = Callable[..., FlashExecutionResult]
TransportFactory = Callable[..., _Transport]
ClientFactory = Callable[[_Transport, float, int], _Client]


def _default_client_factory(transport: _Transport, timeout: float, attempts: int) -> Client:
    return Client(transport, timeout=timeout, max_attempts=attempts)


def _bounded_sleep(sleeper: Sleeper, clock: Clock, deadline: float, requested: float) -> None:
    remaining = deadline - clock()
    if remaining > 0:
        sleeper(min(requested, remaining))


def _drain_to_quiet(
    transport: _Transport,
    *,
    clock: Clock,
    sleeper: Sleeper,
    overall_deadline: float,
) -> bool:
    """Discard raw post-reset bytes until a bounded quiet interval is seen.

    Drained bytes are deliberately never passed to :class:`~hidbot.framing.Framer`:
    this is lexical stream alignment before a new semantic protocol session.
    """

    started = clock()
    drain_deadline = min(overall_deadline, started + MAX_DRAIN_SECONDS)
    quiet_since = started
    discarded = 0
    while True:
        now = clock()
        if now + _CLOCK_EPSILON_SECONDS >= quiet_since + RX_QUIET_SECONDS:
            return True
        if now >= drain_deadline or now >= overall_deadline or discarded >= MAX_DRAIN_BYTES:
            return False
        timeout = min(
            READ_POLL_SECONDS,
            drain_deadline - now,
            overall_deadline - now,
            RX_QUIET_SECONDS - (now - quiet_since),
        )
        if timeout <= _CLOCK_EPSILON_SECONDS:
            return True
        chunk = transport.read(min(READ_CHUNK_BYTES, MAX_DRAIN_BYTES - discarded), timeout)
        if chunk:
            discarded += len(chunk)
            quiet_since = clock()
            continue
        # A real pyserial read normally consumed ``timeout``. The small sleep
        # makes no-data fake transports progress under the same bounded model.
        _bounded_sleep(sleeper, clock, drain_deadline, min(0.01, timeout))


def _close_attempt(client: _Client | None, transport: _Transport | None) -> None:
    """Attempt one close without replacing the primary phase result."""

    if client is not None:
        try:
            client.close()
        except Exception:
            # Preserve the primary phase result.  The direct transport close
            # below still gives a mocked or partly-initialized client no
            # authority to leak an open handle.
            pass
    if transport is not None:
        try:
            # ``Client.close()`` already closes its transport in production;
            # ``PySerialTransport.close()`` is explicitly idempotent.
            transport.close()
        except Exception:
            # A close failure must not conceal a completed flash or a more
            # useful post-flash verification classification.
            pass


def _phase_from_comparison(
    result: FirmwareVerificationResult,
    *,
    reconnect_attempts: int,
    boot_id: str,
) -> VerificationPhaseResult:
    if result.classification is FirmwareVerificationClassification.MATCH:
        classification = VerificationPhaseClassification.MATCH
    elif result.classification is FirmwareVerificationClassification.MISMATCH:
        classification = VerificationPhaseClassification.MISMATCH
    else:
        classification = VerificationPhaseClassification.IDENTITY_UNAVAILABLE
    return VerificationPhaseResult(
        classification=classification,
        reconnect_attempts=reconnect_attempts,
        boot_id=boot_id,
        firmware_verification=result,
    )


def _transient_result(
    classification: VerificationPhaseClassification,
    attempts: int,
) -> VerificationPhaseResult:
    return VerificationPhaseResult(classification=classification, reconnect_attempts=attempts)


def run_post_flash_provisioning(
    bundle: VerifiedFirmwareBundle,
    port: str,
    *,
    flash_executor: FlashExecutor = execute_flash,
    transport_factory: TransportFactory = PySerialTransport,
    client_factory: ClientFactory = _default_client_factory,
    clock: Clock = time.monotonic,
    sleeper: Sleeper = time.sleep,
    json_mode: bool = False,
    on_retry: Callable[[str], None] | None = None,
) -> ProvisioningWorkflowResult:
    """Flash once, then require a fresh bounded runtime identity match.

    ``flash_executor`` retains the B2b programming retry policy internally. It
    is called exactly once here; all later retries are UART readiness attempts
    and cannot re-enter the programming plane.
    """

    flash = flash_executor(bundle, port, json_mode=json_mode, on_retry=on_retry)
    deadline = clock() + READINESS_DEADLINE_SECONDS
    last_transient = VerificationPhaseClassification.TRANSPORT_UNAVAILABLE
    attempts_made = 0

    for reconnect_attempt in range(1, MAX_CONNECTION_ATTEMPTS + 1):
        if clock() >= deadline:
            break
        attempts_made = reconnect_attempt
        transport: _Transport | None = None
        client: _Client | None = None
        try:
            transport = transport_factory(
                port,
                CONTROL_UART_BAUD,
                read_timeout=READ_POLL_SECONDS,
                write_timeout=1.0,
            )
            transport.open()
            if not _drain_to_quiet(
                transport,
                clock=clock,
                sleeper=sleeper,
                overall_deadline=deadline,
            ):
                last_transient = VerificationPhaseClassification.TIMEOUT
                continue

            remaining = deadline - clock()
            if remaining <= 0:
                last_transient = VerificationPhaseClassification.TIMEOUT
                continue
            # ``connect()`` and ``info()`` can each make at most two exact
            # attempts. Clamp each wait so those controllable request waits
            # together fit in the remaining readiness budget.  TTY open is
            # intentionally outside this strict calculation; pyserial has no
            # separate open-timeout contract.
            request_timeout = min(
                REQUEST_TIMEOUT_SECONDS,
                remaining / (CLIENT_MAX_ATTEMPTS * 2),
            )
            if request_timeout <= 0:
                last_transient = VerificationPhaseClassification.TIMEOUT
                continue
            client = client_factory(transport, request_timeout, CLIENT_MAX_ATTEMPTS)
            hello = client.connect()
            info = validate_system_info(client.info(), capabilities=hello.capabilities)
            comparison = compare_firmware_identity(bundle.artifact_identity, hello.capabilities, info)
            return ProvisioningWorkflowResult(
                flash=flash,
                verification=_phase_from_comparison(
                    comparison,
                    reconnect_attempts=reconnect_attempt,
                    boot_id=hello.boot_id,
                ),
            )
        except TransportError:
            last_transient = VerificationPhaseClassification.TRANSPORT_UNAVAILABLE
        except (RequestTimeoutError, SessionLostError):
            last_transient = VerificationPhaseClassification.TIMEOUT
        except CompatibilityError:
            return ProvisioningWorkflowResult(
                flash=flash,
                verification=_transient_result(
                    VerificationPhaseClassification.COMPATIBILITY_ERROR, reconnect_attempt
                ),
            )
        except ProtocolError:
            return ProvisioningWorkflowResult(
                flash=flash,
                verification=_transient_result(
                    VerificationPhaseClassification.PROTOCOL_ERROR, reconnect_attempt
                ),
            )
        except RemoteError:
            return ProvisioningWorkflowResult(
                flash=flash,
                verification=_transient_result(
                    VerificationPhaseClassification.REMOTE_ERROR, reconnect_attempt
                ),
            )
        finally:
            _close_attempt(client, transport)

        if reconnect_attempt < MAX_CONNECTION_ATTEMPTS:
            _bounded_sleep(sleeper, clock, deadline, RECONNECT_INTERVAL_SECONDS)

    return ProvisioningWorkflowResult(
        flash=flash,
        verification=_transient_result(last_transient, attempts_made),
    )
