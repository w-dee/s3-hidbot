"""Small, safe command-line client for the s3-hidbot diagnostic plane."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from collections.abc import Collection, Mapping, Sequence
from dataclasses import asdict
from pathlib import Path
from typing import Any, Callable, TextIO

from .artifact import ArtifactError, verify_bundle_archive, verify_bundle_directory
from .client import Client, HelloResult
from .errors import (
    CompatibilityError,
    FlashExecutionError,
    HidbotError,
    ProtocolError,
    RemoteError,
    RequestTimeoutError,
    SessionLostError,
    TransportError,
)
from .flashing import FlashExecutionResult, execute_flash
from .provisioning_workflow import (
    ProvisioningWorkflowResult,
    VerificationPhaseClassification,
    run_post_flash_provisioning,
)
from .provisioning import stage_and_verify_firmware_bundle
from .protocol import (
    BLE_BOND_ADMINISTRATION_CAPABILITY,
    BLE_PAIRING_TRANSACTION_CAPABILITY,
    BleBondList,
    BleBondRemoveResult,
    BleExposureStatus,
    BlePairingRespondResult,
    BlePairingStatus,
    HidRouteStatus,
    HidRouteV2Status,
    KeyboardReportResult,
    MouseReportResult,
    ReleaseAllResult,
    UsbExposureStatus,
    OutputRouteV2,
    validate_ble_pairing_respond_inputs,
    validate_bond_id,
    validate_keyboard_report_inputs,
    validate_mouse_report_inputs,
    validate_system_info,
)
from .firmware_verification import (
    ArtifactFirmwareIdentity,
    FirmwareVerificationResult,
    artifact_identity_from_verified_manifest,
    compare_firmware_identity,
)


DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 1.0
DEFAULT_ATTEMPTS = 3


def _default_transport_factory(*args: Any, **kwargs: Any) -> Any:
    """Resolve pyserial only when a command actually constructs a transport."""

    from .serial_transport import PySerialTransport

    return PySerialTransport(*args, **kwargs)


class _ExplicitValueAction(argparse.Action):
    """Record whether a value option was explicitly supplied by the caller."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: object,
        option_string: str | None = None,
    ) -> None:
        del parser, option_string
        setattr(namespace, self.dest, values)
        setattr(namespace, f"_{self.dest}_explicit", True)


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


def _raw_hid_integer(value: str) -> int:
    """Parse a decimal or 0x-prefixed integer without introducing symbols."""

    signless = value[1:] if value[:1] in {"+", "-"} else value
    base = 16 if signless.lower().startswith("0x") else 10
    try:
        return int(value, base)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "must be a decimal integer or 0x-prefixed hexadecimal integer"
        ) from exc


def _pairing_id(value: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a decimal uint32 pairing ID") from exc
    if not 1 <= parsed <= 0xFFFF_FFFF:
        raise argparse.ArgumentTypeError("must be in 1..4294967295")
    return parsed


def _bond_id(value: str) -> str:
    try:
        validate_bond_id(value)
    except ProtocolError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return value


def _read_pairing_passkey() -> str:
    """Read from the controlling terminal with echo disabled and no stdin fallback."""

    try:
        import termios

        tty_input = open("/dev/tty", "r", encoding="utf-8", buffering=1)
        try:
            tty_output = open("/dev/tty", "w", encoding="utf-8", buffering=1)
        except OSError:
            tty_input.close()
            raise
    except (ImportError, OSError) as exc:
        raise ProtocolError("a controlling TTY is required for pairing passkey input") from exc
    with tty_input, tty_output:
        try:
            original = termios.tcgetattr(tty_input.fileno())
            hidden = list(original)
            hidden[3] &= ~termios.ECHO
            termios.tcsetattr(tty_input.fileno(), termios.TCSAFLUSH, hidden)
            try:
                tty_output.write("Passkey (6 digits): ")
                tty_output.flush()
                value = tty_input.readline()
            finally:
                termios.tcsetattr(tty_input.fileno(), termios.TCSAFLUSH, original)
                tty_output.write("\n")
                tty_output.flush()
        except (OSError, termios.error) as exc:
            raise ProtocolError("secure controlling-TTY input is unavailable") from exc
    if value == "":
        raise ProtocolError("pairing passkey input was not received")
    if value.endswith("\n"):
        value = value[:-1]
    if value.endswith("\r"):
        value = value[:-1]
    return value


def _add_global_options(
    parser: argparse.ArgumentParser,
    *,
    suppress_defaults: bool = False,
    hidden_help: Collection[str] = (),
) -> None:
    """Register global options while allowing command-specific help curation.

    Hidden actions remain fully parse-compatible.  This is intentionally a
    presentation-only mechanism for commands whose generic options are
    ignored or explicitly rejected by their execution policy.
    """

    default = argparse.SUPPRESS if suppress_defaults else None
    parser.add_argument(
        "--port",
        default=default,
        help=(
            argparse.SUPPRESS
            if "--port" in hidden_help
            else "serial port; otherwise S3_HIDBOT_SERIAL"
        ),
    )
    parser.add_argument(
        "--baud",
        action=_ExplicitValueAction,
        default=default,
        help=(
            argparse.SUPPRESS
            if "--baud" in hidden_help
            else "baud rate; otherwise S3_HIDBOT_BAUD or 115200"
        ),
    )
    parser.add_argument(
        "--timeout",
        action=_ExplicitValueAction,
        default=argparse.SUPPRESS if suppress_defaults else str(DEFAULT_TIMEOUT),
        help=(argparse.SUPPRESS if "--timeout" in hidden_help else "request timeout in seconds"),
    )
    parser.add_argument(
        "--attempts",
        action=_ExplicitValueAction,
        default=argparse.SUPPRESS if suppress_defaults else str(DEFAULT_ATTEMPTS),
        help=(argparse.SUPPRESS if "--attempts" in hidden_help else "maximum request attempts"),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        default=argparse.SUPPRESS if suppress_defaults else False,
        help="emit one compact JSON result",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        default=argparse.SUPPRESS if suppress_defaults else False,
        help="reserved for diagnostic verbosity",
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hidbotctl", description=__doc__)
    _add_global_options(parser)
    commands = parser.add_subparsers(dest="command", required=True)

    for name, help_text in (
        ("hello", "establish a session and show device capabilities"),
        ("ping", "run the bounded diagnostic ping"),
        ("info", "show device information"),
        ("usb-status", "show USB lifecycle and readiness state"),
        ("usb-exposure-status", "show explicit native USB exposure lifecycle state"),
        ("usb-attach", "explicitly install and expose native USB HID"),
        ("usb-detach", "safely hide and uninstall native USB HID"),
        ("ble-exposure-status", "show explicit BLE exposure lifecycle state"),
        ("ble-enable", "explicitly initialize/reuse BLE and advertise"),
        ("ble-disable", "hide BLE while retaining the initialized stack"),
        ("ble-pairing-status", "show the current BLE pairing transaction"),
        ("ble-pairing-respond", "respond to one BLE pairing transaction"),
        ("ble-bond-list", "list firmware-side BLE bonds"),
        ("ble-bond-remove", "remove one exact firmware-side BLE bond"),
        ("hid-route-status", "show the explicit HID output route"),
        ("hid-route-set", "select none, USB, or BLE as the HID output route"),
        ("release-all", "perform the safe all-up recovery operation"),
        (
            "self-test",
            "run hello/ping/info/usb-status/release-all safely; does not prove HID delivery",
        ),
        (
            "verify-firmware",
            "compare a verified firmware artifact with the connected device identity",
        ),
        (
            "verify-artifact",
            "verify a firmware artifact locally without connecting to a device",
        ),
        (
            "flash-firmware",
            "flash a verified firmware artifact using the supported provisioning policy",
        ),
    ):
        command = commands.add_parser(name, help=help_text, description=help_text)
        hidden_help: Collection[str] = ()
        if name == "flash-firmware":
            hidden_help = {"--baud", "--timeout", "--attempts"}
        elif name == "verify-artifact":
            hidden_help = {"--port", "--baud", "--timeout", "--attempts"}
        _add_global_options(
            command,
            suppress_defaults=True,
            hidden_help=hidden_help,
        )
        if name == "hid-route-set":
            command.add_argument(
                "route",
                choices=[route.value for route in OutputRouteV2],
                help="explicit HID output route",
            )
        if name == "ble-pairing-respond":
            command.add_argument(
                "--pairing-id",
                type=_pairing_id,
                required=True,
                metavar="ID",
                help="nonzero pairing transaction ID from ble-pairing-status",
            )
        if name == "ble-bond-remove":
            command.add_argument(
                "bond_id",
                type=_bond_id,
                metavar="BOND_ID",
                help="exact 32-lowercase-hex ID from ble-bond-list",
            )
        if name in {"verify-artifact", "verify-firmware", "flash-firmware"}:
            command.add_argument(
                "artifact",
                metavar="ARTIFACT",
                help="verified firmware bundle archive or extracted bundle directory",
            )
    keyboard = commands.add_parser(
        "keyboard-report",
        help="submit one explicit unsafe Boot keyboard report",
        description="submit one explicit unsafe Boot keyboard report",
    )
    _add_global_options(keyboard, suppress_defaults=True)
    keyboard.add_argument(
        "--unsafe-hid",
        action="store_true",
        required=True,
        help="required opt-in for an unsafe HID report",
    )
    keyboard.add_argument(
        "--modifiers",
        type=_raw_hid_integer,
        required=True,
        metavar="N",
        help="modifier bitmap (decimal or 0xNN)",
    )
    keyboard.add_argument(
        "--key",
        dest="keys",
        type=_raw_hid_integer,
        action="append",
        default=[],
        metavar="USAGE",
        help="raw HID usage, repeatable up to six times (decimal or 0xNN)",
    )

    mouse = commands.add_parser(
        "mouse-report",
        help="submit one explicit unsafe Boot mouse report",
        description="submit one explicit unsafe Boot mouse report",
    )
    _add_global_options(mouse, suppress_defaults=True)
    mouse.add_argument(
        "--unsafe-hid",
        action="store_true",
        required=True,
        help="required opt-in for an unsafe HID report",
    )
    for name, help_text in (
        ("buttons", "absolute persistent button bitmap (0..31)"),
        ("x", "relative X delta (-127..127)"),
        ("y", "relative Y delta (-127..127)"),
        ("wheel", "relative vertical wheel delta (-127..127)"),
        ("pan", "relative horizontal pan delta (-127..127)"),
    ):
        mouse.add_argument(
            f"--{name}",
            type=_raw_hid_integer,
            required=True,
            metavar="N",
            help=f"{help_text}; decimal or 0xNN",
        )
    return parser


def _hello_value(result: HelloResult) -> dict[str, Any]:
    value = asdict(result)
    value["capabilities"] = list(result.capabilities)
    return value


def _result_value(command: str, result: object) -> object:
    if command == "hello":
        assert isinstance(result, HelloResult)
        return _hello_value(result)
    if command == "release-all":
        assert isinstance(result, ReleaseAllResult)
        return asdict(result)
    if command == "keyboard-report":
        assert isinstance(result, KeyboardReportResult)
        return asdict(result)
    if command == "mouse-report":
        assert isinstance(result, MouseReportResult)
        return asdict(result)
    if command in {"usb-attach", "usb-detach", "usb-exposure-status"}:
        assert isinstance(result, UsbExposureStatus)
        return asdict(result)
    if command in {"ble-exposure-status", "ble-enable", "ble-disable"}:
        assert isinstance(result, BleExposureStatus)
        return asdict(result)
    if command == "ble-pairing-status":
        assert isinstance(result, BlePairingStatus)
        value = asdict(result)
        value["state"] = result.state.value
        value["action"] = result.action.value if result.action is not None else None
        value["last_result"] = result.last_result.value
        return value
    if command == "ble-pairing-respond":
        assert isinstance(result, BlePairingRespondResult)
        return asdict(result)
    if command == "ble-bond-list":
        assert isinstance(result, BleBondList)
        value = asdict(result)
        value["bonds"] = [asdict(bond) for bond in result.bonds]
        return value
    if command == "ble-bond-remove":
        assert isinstance(result, BleBondRemoveResult)
        return asdict(result)
    if command in {"hid-route-status", "hid-route-set"}:
        assert isinstance(result, (HidRouteStatus, HidRouteV2Status))
        return asdict(result)
    return result


def _verified_artifact_identity(value: str) -> ArtifactFirmwareIdentity:
    """Verify an artifact before serial setup, then extract its identity once."""

    path = Path(value)
    if path.is_dir():
        manifest = verify_bundle_directory(path)
    elif path.is_file():
        manifest = verify_bundle_archive(path)
    else:
        raise ArtifactError("artifact path must be an existing bundle directory or archive file")
    return artifact_identity_from_verified_manifest(manifest)


def _artifact_identity_value(value: ArtifactFirmwareIdentity) -> dict[str, object]:
    return {
        "project": value.project,
        "target": value.target,
        "protocol_version": value.protocol_version,
        "version": value.version,
        "source_revision": value.source_revision,
        "app_elf_sha256": value.app_elf_sha256,
        "build_profile": value.build_profile,
        "idf_version": value.idf_version,
    }


def _device_identity_value(result: FirmwareVerificationResult) -> dict[str, object | None]:
    firmware = result.device.firmware
    return {
        "project": result.device.project,
        "target": result.device.target,
        "protocol_version": result.device.protocol_version,
        "version": None if firmware is None else firmware.version,
        "source_revision": None if firmware is None else firmware.source_revision,
        "app_elf_sha256": None if firmware is None else firmware.app_elf_sha256,
        "build_profile": None if firmware is None else firmware.build_profile,
        "idf_version": result.device.idf_version,
    }


def _firmware_verification_value(result: FirmwareVerificationResult) -> dict[str, object]:
    """Render one stable CLI schema without exposing manifest or wire payloads."""

    return {
        "ok": True,
        "match": result.match,
        "classification": result.classification.value,
        "artifact": _artifact_identity_value(result.artifact),
        "device": _device_identity_value(result),
        "mismatches": [mismatch.value for mismatch in result.mismatches],
        "unavailable_reason": (
            None if result.unavailable_reason is None else result.unavailable_reason.value
        ),
    }


def _artifact_validation_value(result: ArtifactFirmwareIdentity) -> dict[str, object]:
    """Render the stable artifact-only result without device or serial state."""

    return {
        "ok": True,
        "classification": "VALID",
        "artifact": _artifact_identity_value(result),
    }


def _flash_value(
    identity: ArtifactFirmwareIdentity, result: ProvisioningWorkflowResult
) -> dict[str, object]:
    verification = result.verification
    comparison = verification.firmware_verification
    return {
        "ok": result.ok,
        "classification": result.classification.value,
        "artifact": _artifact_identity_value(identity),
        "flash": {
            "classification": result.flash_classification.value,
            "chip": result.flash.chip,
            "image_count": result.flash.image_count,
            "attempts": result.flash.attempts,
        },
        "verification": {
            "classification": verification.classification.value,
            "match": verification.match,
            "boot_id": verification.boot_id,
            "reconnect_attempts": verification.reconnect_attempts,
            "device": None if comparison is None else _device_identity_value(comparison),
            "mismatches": (
                [] if comparison is None else [mismatch.value for mismatch in comparison.mismatches]
            ),
            "unavailable_reason": (
                None
                if comparison is None or comparison.unavailable_reason is None
                else comparison.unavailable_reason.value
            ),
        },
    }


def _flash_exit_code(result: ProvisioningWorkflowResult) -> int:
    classification = result.verification.classification
    if classification is VerificationPhaseClassification.MATCH:
        return 0
    if classification in {
        VerificationPhaseClassification.MISMATCH,
        VerificationPhaseClassification.IDENTITY_UNAVAILABLE,
    }:
        return 7
    if classification is VerificationPhaseClassification.TRANSPORT_UNAVAILABLE:
        return 3
    if classification is VerificationPhaseClassification.REMOTE_ERROR:
        return 5
    if classification is VerificationPhaseClassification.TIMEOUT:
        return 6
    return 4


def _print_flash_result(value: dict[str, object], *, as_json: bool, output: TextIO) -> None:
    if as_json:
        print(json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True), file=output)
        return
    flash = value["flash"]
    verification = value["verification"]
    assert isinstance(flash, dict)
    assert isinstance(verification, dict)
    print("firmware flash: FLASHED", file=output)
    print(f"chip: {flash['chip']}", file=output)
    print(f"images: {flash['image_count']}", file=output)
    print(f"flash attempts: {flash['attempts']}", file=output)
    print(f"post-flash verification: {verification['classification']}", file=output)
    print(f"reconnect attempts: {verification['reconnect_attempts']}", file=output)
    if verification["boot_id"] is not None:
        print(f"boot_id: {verification['boot_id']}", file=output)
    for mismatch in verification["mismatches"]:
        print(f"mismatch: {mismatch}", file=output)
    if verification["unavailable_reason"] is not None:
        print(f"unavailable_reason: {verification['unavailable_reason']}", file=output)


def _validate_hid_arguments(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> None:
    """Validate unsafe report inputs before any transport is constructed."""

    try:
        if args.command == "keyboard-report":
            args.keys = validate_keyboard_report_inputs(args.modifiers, args.keys)
        elif args.command == "mouse-report":
            validate_mouse_report_inputs(
                args.buttons, args.x, args.y, args.wheel, args.pan
            )
    except ProtocolError as exc:
        parser.error(str(exc))


def _validate_flash_arguments(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if args.command != "flash-firmware":
        return
    rejected = [
        option
        for option, destination in (
            ("--baud", "baud"),
            ("--timeout", "timeout"),
            ("--attempts", "attempts"),
        )
        if getattr(args, f"_{destination}_explicit", False)
    ]
    if rejected:
        parser.error(
            "flash-firmware does not accept "
            + ", ".join(rejected)
            + "; flashing uses fixed programming settings"
        )


def _print_result(command: str, result: object, *, as_json: bool, output: TextIO) -> None:
    if command == "verify-artifact":
        assert isinstance(result, ArtifactFirmwareIdentity)
        value = _artifact_validation_value(result)
        if as_json:
            print(json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True), file=output)
            return
        print("firmware artifact: VALID", file=output)
        print("artifact:", file=output)
        print(json.dumps(value["artifact"], ensure_ascii=True, indent=2, sort_keys=True), file=output)
        return
    if command == "verify-firmware":
        assert isinstance(result, FirmwareVerificationResult)
        value = _firmware_verification_value(result)
        if as_json:
            print(json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True), file=output)
            return
        print(f"firmware identity: {value['classification']}", file=output)
        if value["classification"] == "MATCH":
            return
        print("artifact:", file=output)
        print(json.dumps(value["artifact"], ensure_ascii=True, indent=2, sort_keys=True), file=output)
        print("device:", file=output)
        print(json.dumps(value["device"], ensure_ascii=True, indent=2, sort_keys=True), file=output)
        if value["unavailable_reason"] is not None:
            print(f"unavailable_reason: {value['unavailable_reason']}", file=output)
        for mismatch in value["mismatches"]:
            print(f"mismatch: {mismatch}", file=output)
        return
    value = _result_value(command, result)
    if as_json:
        print(json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True), file=output)
        return
    if command == "hello":
        assert isinstance(value, dict)
        print(f"session: {value['session']}", file=output)
        print(f"boot_id: {value['boot_id']}", file=output)
        print(f"client_nonce: {value['client_nonce']}", file=output)
        print(f"lease_ms: {value['lease_ms']}", file=output)
        print(f"capabilities: {', '.join(value['capabilities'])}", file=output)
        return
    if command == "ble-pairing-status":
        assert isinstance(value, dict)
        print(f"state: {value['state']}", file=output)
        if value["pairing_id"] is not None:
            print(f"pairing_id: {value['pairing_id']}", file=output)
            print(f"remaining_ms: {value['remaining_ms']}", file=output)
        print(f"connected: {value['connected']}", file=output)
        print(f"encrypted: {value['encrypted']}", file=output)
        print(f"authenticated: {value['authenticated']}", file=output)
        print(f"bonded: {value['bonded']}", file=output)
        print(f"secure_connections: {value['secure_connections']}", file=output)
        print(f"key_size: {value['key_size']}", file=output)
        print(f"last_result: {value['last_result']}", file=output)
        return
    if command == "ble-bond-list":
        assert isinstance(value, dict)
        print(
            f"bonds: {value['count']}/{value['capacity']} "
            f"(available={value['available']}, healthy={value['healthy']})",
            file=output,
        )
        for bond in value["bonds"]:
            print(
                f"bond_id: {bond['bond_id']} verified={bond['verified']} "
                f"schema_revision={bond['schema_revision']} "
                f"schema_current={bond['schema_current']} "
                f"connected={bond['connected']}",
                file=output,
            )
        return
    print(json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True), file=output)


def _exit_code(error: HidbotError) -> int:
    if isinstance(error, RequestTimeoutError):
        return 6
    if isinstance(error, FlashExecutionError):
        return 8
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
    transport_factory: Callable[..., Any] = _default_transport_factory,
    flash_executor: Callable[..., FlashExecutionResult] = execute_flash,
    provisioning_workflow_runner: Callable[..., ProvisioningWorkflowResult] = run_post_flash_provisioning,
    output: TextIO | None = None,
    error_output: TextIO | None = None,
    passkey_reader: Callable[[], str] | None = None,
) -> int:
    output = sys.stdout if output is None else output
    error_output = sys.stderr if error_output is None else error_output
    parser = _parser()
    try:
        raw_arguments = tuple(sys.argv[1:] if argv is None else argv)
        if any(
            argument == "--passkey" or argument.startswith("--passkey=")
            for argument in raw_arguments
        ):
            parser.error("passkey must be entered at the controlling-TTY prompt")
        args = parser.parse_args(raw_arguments)
        _validate_hid_arguments(args, parser)
        _validate_flash_arguments(args, parser)
        artifact_identity = (
            _verified_artifact_identity(args.artifact)
            if args.command in {"verify-artifact", "verify-firmware"}
            else None
        )
        if args.command == "verify-artifact":
            assert artifact_identity is not None
            _print_result(args.command, artifact_identity, as_json=args.json, output=output)
            return 0
        if args.command == "flash-firmware":
            port = resolve_port(args.port, environ)
            with stage_and_verify_firmware_bundle(args.artifact) as bundle:
                result = provisioning_workflow_runner(
                    bundle,
                    port,
                    flash_executor=flash_executor,
                    transport_factory=transport_factory,
                    json_mode=args.json,
                    on_retry=lambda message: print(message, file=error_output),
                )
                value = _flash_value(bundle.artifact_identity, result)
                _print_flash_result(value, as_json=args.json, output=output)
            return _flash_exit_code(result)
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
            elif args.command == "usb-status":
                result = client.usb_status()
            elif args.command == "usb-exposure-status":
                result = client.usb_exposure_status()
            elif args.command == "usb-attach":
                result = client.usb_attach()
            elif args.command == "usb-detach":
                result = client.usb_detach()
            elif args.command == "ble-exposure-status":
                result = client.ble_exposure_status()
            elif args.command == "ble-enable":
                result = client.ble_enable()
            elif args.command == "ble-disable":
                result = client.ble_disable()
            elif args.command == "ble-pairing-status":
                result = client.ble_pairing_status()
            elif args.command == "ble-pairing-respond":
                if BLE_PAIRING_TRANSACTION_CAPABILITY not in hello.capabilities:
                    raise CompatibilityError(
                        f"peer does not advertise {BLE_PAIRING_TRANSACTION_CAPABILITY}"
                    )
                passkey = (
                    _read_pairing_passkey()
                    if passkey_reader is None
                    else passkey_reader()
                )
                try:
                    validate_ble_pairing_respond_inputs(args.pairing_id, passkey)
                    result = client.ble_pairing_respond(args.pairing_id, passkey)
                finally:
                    # Python strings are immutable; release the CLI's local
                    # reference immediately after the single Client call.
                    passkey = ""
            elif args.command == "ble-bond-list":
                result = client.ble_bond_list()
            elif args.command == "ble-bond-remove":
                if BLE_BOND_ADMINISTRATION_CAPABILITY not in hello.capabilities:
                    raise CompatibilityError(
                        f"peer does not advertise {BLE_BOND_ADMINISTRATION_CAPABILITY}"
                    )
                result = client.ble_bond_remove(args.bond_id)
            elif args.command == "hid-route-status":
                result = client.hid_route_status()
            elif args.command == "hid-route-set":
                result = client.hid_route_set(args.route)
            elif args.command == "release-all":
                result = client.release_all()
            elif args.command == "self-test":
                result = {
                    "hello": _hello_value(hello),
                    "ping": client.ping(),
                    "info": client.info(),
                    "usb_status": client.usb_status(),
                    "release_all": asdict(client.release_all()),
                }
            elif args.command == "verify-firmware":
                assert artifact_identity is not None
                info = validate_system_info(client.info(), capabilities=hello.capabilities)
                result = compare_firmware_identity(artifact_identity, hello.capabilities, info)
            elif args.command == "keyboard-report":
                result = client.keyboard_report(args.modifiers, args.keys)
            else:
                assert args.command == "mouse-report"
                result = client.mouse_report(
                    args.buttons, args.x, args.y, args.wheel, args.pan
                )
            _print_result(args.command, result, as_json=args.json, output=output)
            if args.command == "verify-firmware":
                assert isinstance(result, FirmwareVerificationResult)
                return 0 if result.match else 7
            return 0
        finally:
            if client is not None:
                client.close()
            else:
                transport.close()
    except ArtifactError as exc:
        print(f"artifact error: {exc}", file=error_output)
        return 2
    except ValueError as exc:
        print(f"configuration error: {exc}", file=error_output)
        return 2
    except HidbotError as exc:
        print(f"error: {exc}", file=error_output)
        if isinstance(exc, FlashExecutionError) and exc.diagnostic_tail:
            print(exc.diagnostic_tail.decode("utf-8", errors="replace").rstrip("\n"), file=error_output)
        return _exit_code(exc)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
