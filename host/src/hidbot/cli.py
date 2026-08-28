"""Small, safe command-line client for the s3-hidbot diagnostic plane."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from collections.abc import Mapping, Sequence
from dataclasses import asdict
from typing import Any, Callable, TextIO

from .client import Client, HelloResult
from .errors import (
    CompatibilityError,
    HidbotError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .serial_transport import PySerialTransport


DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 1.0
DEFAULT_ATTEMPTS = 3


def resolve_port(cli_port: str | None, environ: Mapping[str, str] | None = None) -> str:
    """Resolve an explicit port before the machine-local environment value."""

    environment = os.environ if environ is None else environ
    port = cli_port or environment.get("S3_HIDBOT_SERIAL")
    if not port:
        raise ValueError("serial port is required via --port or S3_HIDBOT_SERIAL")
    return port


def _positive_int(value: str, field: str) -> int:
    try:
        parsed = int(value, 10)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be a positive integer") from exc
    if parsed <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return parsed


def resolve_baud(cli_baud: str | None, environ: Mapping[str, str] | None = None) -> int:
    environment = os.environ if environ is None else environ
    raw = cli_baud if cli_baud is not None else environment.get("S3_HIDBOT_BAUD", str(DEFAULT_BAUD))
    return _positive_int(raw, "baud")


def _positive_float(value: str, field: str) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be positive and finite") from exc
    if not math.isfinite(parsed) or parsed <= 0:
        raise ValueError(f"{field} must be positive and finite")
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hidbotctl", description=__doc__)
    parser.add_argument("--port", help="serial port; otherwise S3_HIDBOT_SERIAL")
    parser.add_argument("--baud", help="baud rate; otherwise S3_HIDBOT_BAUD or 115200")
    parser.add_argument("--timeout", default=str(DEFAULT_TIMEOUT), help="request timeout in seconds")
    parser.add_argument("--attempts", default=str(DEFAULT_ATTEMPTS), help="maximum request attempts")
    parser.add_argument("--json", action="store_true", help="emit one compact JSON result")
    parser.add_argument("--verbose", action="store_true", help="reserved for diagnostic verbosity")
    parser.add_argument("command", choices=("hello", "ping", "info", "usb-status"))
    return parser


def _hello_value(result: HelloResult) -> dict[str, Any]:
    value = asdict(result)
    value["capabilities"] = list(result.capabilities)
    return value


def _result_value(command: str, result: object) -> object:
    if command == "hello":
        assert isinstance(result, HelloResult)
        return _hello_value(result)
    return result


def _print_result(command: str, result: object, *, as_json: bool, output: TextIO) -> None:
    value = _result_value(command, result)
    if as_json:
        print(json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True), file=output)
        return
    if command == "hello":
        assert isinstance(value, dict)
        print(f"session: {value['session']}", file=output)
        print(f"boot_id: {value['boot_id']}", file=output)
        print(f"client_nonce: {value['client_nonce']}", file=output)
        print(f"capabilities: {', '.join(value['capabilities'])}", file=output)
        return
    print(json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True), file=output)


def _exit_code(error: HidbotError) -> int:
    if isinstance(error, RequestTimeoutError):
        return 6
    if isinstance(error, RemoteError):
        return 5
    if isinstance(error, TransportError):
        return 3
    if isinstance(error, (CompatibilityError, ProtocolError, SessionLostError)):
        return 4
    return 4


def main(
    argv: Sequence[str] | None = None,
    *,
    environ: Mapping[str, str] | None = None,
    transport_factory: Callable[..., PySerialTransport] = PySerialTransport,
    output: TextIO | None = None,
    error_output: TextIO | None = None,
) -> int:
    output = sys.stdout if output is None else output
    error_output = sys.stderr if error_output is None else error_output
    parser = _parser()
    try:
        args = parser.parse_args(argv)
        port = resolve_port(args.port, environ)
        baud = resolve_baud(args.baud, environ)
        timeout = _positive_float(args.timeout, "timeout")
        attempts = _positive_int(args.attempts, "attempts")
        transport = transport_factory(
            port,
            baud,
            read_timeout=min(0.05, timeout),
            write_timeout=1.0,
        )
        client: Client | None = None
        try:
            transport.open()
            client = Client(transport, timeout=timeout, max_attempts=attempts)
            hello = client.connect()
            if args.command == "hello":
                result: object = hello
            elif args.command == "ping":
                result = client.ping()
            elif args.command == "info":
                result = client.info()
            else:
                result = client.usb_status()
            _print_result(args.command, result, as_json=args.json, output=output)
            return 0
        finally:
            if client is not None:
                client.close()
            else:
                transport.close()
    except ValueError as exc:
        print(f"configuration error: {exc}", file=error_output)
        return 2
    except HidbotError as exc:
        print(f"error: {exc}", file=error_output)
        return _exit_code(exc)


if __name__ == "__main__":
    raise SystemExit(main())
