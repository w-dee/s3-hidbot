"""Small, structured exception taxonomy for the host control core."""

from __future__ import annotations

from typing import Any


class HidbotError(Exception):
    """Base class for errors raised by the host control-plane core."""


class TransportError(HidbotError):
    """The byte transport could not read, write, or close successfully."""


class RequestTimeoutError(HidbotError):
    """A bounded request deadline expired without a correlated response."""

    def __init__(
        self,
        message: str,
        *,
        request_id: int,
        attempts: int,
        diagnostics: tuple[str, ...] = (),
    ) -> None:
        super().__init__(message)
        self.request_id = request_id
        self.attempts = attempts
        self.diagnostics = diagnostics


class ProtocolError(HidbotError):
    """The received or requested wire data violates the v1 contract."""


class CompatibilityError(ProtocolError):
    """The peer answered, but does not implement the required v1 contract."""


class SessionLostError(HidbotError):
    """The active control session can no longer be used safely."""

    def __init__(self, message: str, *, request_id: int | None = None) -> None:
        super().__init__(message)
        self.request_id = request_id


class RemoteError(HidbotError):
    """A schema-valid error response correlated to the active request."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        request_id: int,
        session: str,
        details: Any = None,
    ) -> None:
        super().__init__(f"remote error {code}: {message}")
        self.code = code
        self.message = message
        self.request_id = request_id
        self.session = session
        self.details = details
