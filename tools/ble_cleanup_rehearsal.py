#!/usr/bin/env python3
"""Opt-in, cleanup-only BLE hardware rehearsal for the qualification harness.

This runner never starts a HID sequence or sends a normal keyboard/mouse
report.  It exists to exercise the repository-owned BLE cleanup state machine
against an explicitly identified engineering artifact and an existing retained bond.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import errno
import fcntl
import re
import hashlib
import json
import os
from pathlib import Path
import selectors
import struct
import subprocess
import sys
import time
from typing import Any, Callable


REPOSITORY = Path(
    os.environ.get("S3_HIDBOT_REPOSITORY_ROOT", Path(__file__).resolve().parents[1])
).resolve()
sys.path.insert(0, str(REPOSITORY / "tools"))
sys.path.insert(0, str(REPOSITORY / "host" / "src"))

from hidbot.client import Client  # noqa: E402
from hidbot.errors import SessionLostError  # noqa: E402
from hidbot.firmware_verification import compare_firmware_identity  # noqa: E402
from hidbot.protocol import OutputRouteV2, validate_system_info  # noqa: E402
from hidbot.provisioning import stage_and_inspect_firmware_bundle  # noqa: E402
from hidbot.serial_transport import PySerialTransport  # noqa: E402
from qualification_harness import (  # noqa: E402
    BleCleanupPhase,
    FinalBleControlState,
    ObserverOutcome,
    QualificationError,
    classify_observer_error,
    observation_complete,
    run_ble_cleanup,
)


BUILD_PROFILE = "freenove-fnk0099"
CLASSIFICATION = "CLEANUP_REHEARSAL_ONLY / NOT_QUALIFICATION"

EV_SYN = 0x00
EV_KEY = 0x01
EV_REL = 0x02
EV_MSC = 0x04
SYN_DROPPED = 0x03
MSC_SCAN = 0x04
REL_X = 0
KEY_F24 = 194
BTN_LEFT = 272


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return None


def _ancestor_text(start: Path, name: str) -> str | None:
    for path in (start, *start.parents):
        value = _read(path / name)
        if value:
            return value
        if path == Path("/sys"):
            break
    return None


def _capability(event: Path, group: str, bit: int) -> bool:
    value = _read(event / "device" / "capabilities" / group)
    if value is None:
        return False
    try:
        width = struct.calcsize("L") * 2
        words = value.replace(",", " ").split()
        bits = int("".join(word.zfill(width) for word in words), 16)
    except ValueError:
        return False
    return bool(bits & (1 << bit))


def _normalize_address(value: str) -> str:
    return "".join(character for character in value.lower() if character in "0123456789abcdef")


def _headless() -> dict[str, object]:
    active = subprocess.run(
        ["systemctl", "is-active", "lightdm"], capture_output=True, text=True, timeout=5
    ).stdout.strip()
    enabled = subprocess.run(
        ["systemctl", "is-enabled", "lightdm"], capture_output=True, text=True, timeout=5
    ).stdout.strip()
    names = subprocess.run(
        ["ps", "-eo", "comm="], check=True, capture_output=True, text=True, timeout=5
    ).stdout.lower().splitlines()
    graphical = sum(
        any(token in name for token in ("wayfire", "weston", "sway", "xorg", "xwayland"))
        for name in names
    )
    return {
        "lightdm_active": active,
        "lightdm_enabled": enabled,
        "graphical_consumers": graphical,
        "safe": active == "inactive" and enabled == "disabled" and graphical == 0,
    }


def _serial_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = sorted(
        path
        for path in Path("/dev/serial/by-id").glob("*")
        if path.is_symlink() and "USB_Single_Serial" in path.name
    )
    if len(candidates) != 1:
        raise QualificationError("exactly one control serial device is required")
    return str(candidates[0])


def _run_bluetoothctl(*arguments: str, timeout: float = 10) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bluetoothctl", *arguments],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def _adapter_state() -> dict[str, object]:
    listed = _run_bluetoothctl("list", timeout=5)
    shown = _run_bluetoothctl("show", timeout=5)
    adapters = [line for line in listed.stdout.splitlines() if line.strip().startswith("Controller ")]
    properties: dict[str, str] = {}
    for raw in shown.stdout.splitlines():
        line = raw.strip()
        if ": " in line:
            key, value = line.split(": ", 1)
            properties[key] = value
    return {
        "adapter_count": len(adapters),
        "readable": listed.returncode == 0 and shown.returncode == 0,
        "powered": properties.get("Powered") == "yes",
        "pairable": properties.get("Pairable") == "yes",
        "discoverable": properties.get("Discoverable") == "yes",
    }


def _paired_target() -> tuple[str, dict[str, object]]:
    listed = _run_bluetoothctl("devices", "Paired", timeout=5)
    if listed.returncode != 0:
        listed = _run_bluetoothctl("paired-devices", timeout=5)
    addresses: list[str] = []
    for line in listed.stdout.splitlines():
        fields = line.strip().split(maxsplit=2)
        if len(fields) >= 2 and fields[0] == "Device":
            addresses.append(fields[1])
    candidates: list[tuple[str, bool]] = []
    for address in sorted(set(addresses)):
        info = _run_bluetoothctl("info", address, timeout=5)
        properties: dict[str, str] = {}
        for raw in info.stdout.splitlines():
            line = raw.strip()
            if ": " in line:
                key, value = line.split(": ", 1)
                properties[key] = value
        if (
            info.returncode == 0
            and (properties.get("Name") == "s3-hidbot" or properties.get("Alias") == "s3-hidbot")
            and properties.get("Paired") == "yes"
            and properties.get("Bonded", "yes") == "yes"
        ):
            candidates.append((address, properties.get("Connected") == "yes"))
    if len(candidates) != 1:
        raise QualificationError("exactly one retained s3-hidbot bond is required")
    return candidates[0][0], {
        "target_records": 1,
        "paired": True,
        "bonded": True,
        "connected": candidates[0][1],
    }


def _bluez_connected(address: str) -> bool:
    info = _run_bluetoothctl("info", address, timeout=5)
    return info.returncode == 0 and "Connected: yes" in info.stdout


def _connect_target(address: str, intent: Callable[[str], None]) -> None:
    if _bluez_connected(address):
        return
    intent("Connect")
    _run_bluetoothctl("--timeout", "12", "connect", address, timeout=15)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if _bluez_connected(address):
            return
        time.sleep(0.1)
    raise QualificationError("retained BLE target did not connect")


class Event:
    record = struct.Struct("@llHHi")

    def __init__(self, raw: tuple[int, int, int, int, int]) -> None:
        self.seconds, self.microseconds, self.event_type, self.code, self.value = raw


class PendingEvidenceError(OSError):
    """Terminal observer error carrying complete records decoded before it."""

    def __init__(self, error: OSError, events: list[Event]) -> None:
        super().__init__(error.errno, error.strerror)
        self.events = events


class Observer:
    def __init__(self, path: Path, role: str) -> None:
        self.path = path
        self.role = role
        self.fd: int | None = None
        self.pending = bytearray()

    def open(self) -> None:
        self.fd = os.open(
            self.path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)
        )

    def _decode_pending(self) -> list[Event]:
        events: list[Event] = []
        while len(self.pending) >= Event.record.size:
            raw = Event.record.unpack(self.pending[: Event.record.size])
            event = Event(raw)
            if event.event_type == EV_SYN and event.code == SYN_DROPPED:
                # Return any earlier records first.  The next call consumes
                # this terminal evidence before waiting for more fd input.
                if events:
                    return events
                del self.pending[: Event.record.size]
                raise OSError(errno.EIO, "evdev synchronization dropped")
            del self.pending[: Event.record.size]
            events.append(event)
        return events

    def finish_pending(self) -> list[Event]:
        """Return every complete pending record or fail on terminal evidence."""
        events: list[Event] = []
        while self.pending:
            try:
                decoded = self._decode_pending()
            except OSError as exc:
                raise PendingEvidenceError(exc, events) from exc
            events.extend(decoded)
            if decoded:
                continue
            # No complete record remains, so any bytes here are an
            # unadjudicated partial record. A collection boundary may not
            # silently turn those bytes into an ALL_UP observation.
            raise PendingEvidenceError(
                OSError(errno.EIO, "partial evdev record"), events
            )
        return events

    def read(self, timeout: float) -> list[Event]:
        if self.fd is None:
            raise OSError(errno.ENODEV, "observer is closed")
        events = self._decode_pending()
        if events:
            return events
        with selectors.DefaultSelector() as selector:
            selector.register(self.fd, selectors.EVENT_READ)
            if not selector.select(max(0.0, timeout)):
                return []
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                if self.pending:
                    raise OSError(errno.EIO, "partial evdev record")
                break
            except OSError as exc:
                if exc.errno in {errno.EAGAIN, errno.EWOULDBLOCK}:
                    if self.pending:
                        raise OSError(errno.EIO, "partial evdev record")
                    break
                if self.pending:
                    raise OSError(errno.EIO, "partial evdev record") from exc
                raise
            if not data:
                if self.pending:
                    raise OSError(errno.EIO, "partial evdev record")
                raise OSError(errno.ENODEV, "exact evdev observer retired")
            self.pending.extend(data)
            events = self._decode_pending()
            if events:
                # Complete records become caller-owned evidence before another
                # read can report terminal device lifetime.
                return events
        return events

    def held_count(self) -> int:
        if self.fd is None:
            raise OSError(errno.ENODEV, "observer is closed")
        # Linux EVIOCGKEY(96): query all KEY_MAX bits, including state held
        # before this fd was opened. Empty event history alone is not ALL_UP.
        key_bits = bytearray(96)
        fcntl.ioctl(self.fd, (2 << 30) | (len(key_bits) << 16) | (ord("E") << 8) | 0x18,
                    key_bits, True)
        return sum(byte.bit_count() for byte in key_bits)

    def close(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class ExactObservers:
    def __init__(self, keyboard: Observer, mouse: Observer) -> None:
        self.observers = (keyboard, mouse)
        self.held_keys: set[int] = set()
        self.held_buttons: set[int] = set()

    @classmethod
    def discover(cls, address: str) -> "ExactObservers":
        exact = _normalize_address(address)
        keyboards: list[Path] = []
        mice: list[Path] = []
        for node in sorted(Path("/sys/class/input").glob("event[0-9]*")):
            try:
                resolved = node.resolve(strict=True)
            except OSError:
                continue
            unique = _ancestor_text(resolved, "uniq") or _read(node / "device" / "uniq")
            if not unique or _normalize_address(unique) != exact:
                continue
            device = Path("/dev/input") / node.name
            if not device.exists():
                continue
            if _capability(node, "key", KEY_F24):
                keyboards.append(device)
            if _capability(node, "key", BTN_LEFT) and _capability(node, "rel", REL_X):
                mice.append(device)
        if len(keyboards) != 1 or len(mice) != 1 or keyboards[0] == mice[0]:
            raise QualificationError("exact BLE keyboard and mouse observers are required")
        result = cls(Observer(keyboards[0], "keyboard"), Observer(mice[0], "mouse"))
        for observer in result.observers:
            observer.open()
        return result

    def collect(self, duration: float, *, phase: BleCleanupPhase, retirement: bool) -> ObserverOutcome:
        deadline = time.monotonic() + duration
        relevant = 0
        unexpected = 0
        retired = [False] * len(self.observers)
        retirement_error: OSError | None = None

        def aggregate(observer: Observer, events: list[Event]) -> None:
            nonlocal relevant, unexpected
            for event in events:
                if event.event_type == EV_SYN:
                    continue
                if event.event_type == EV_MSC and event.code == MSC_SCAN:
                    continue
                if (
                    observer.role == "keyboard"
                    and event.event_type == EV_KEY
                    and event.code == KEY_F24
                    and event.value in {0, 1, 2}
                ):
                    relevant += 1
                    if event.value == 1:
                        self.held_keys.add(event.code)
                    elif event.value == 0:
                        self.held_keys.discard(event.code)
                elif (
                    observer.role == "mouse"
                    and event.event_type == EV_KEY
                    and event.code == BTN_LEFT
                    and event.value in {0, 1}
                ):
                    relevant += 1
                    if event.value == 1:
                        self.held_buttons.add(event.code)
                    else:
                        self.held_buttons.discard(event.code)
                else:
                    unexpected += 1

        def finish_pending() -> OSError | None:
            pending_error: OSError | None = None
            for observer in self.observers:
                try:
                    aggregate(observer, observer.finish_pending())
                except OSError as exc:
                    aggregate(observer, getattr(exc, "events", []))
                    # Continue adjudicating the other observer so already
                    # complete evidence is never blanked by the first error.
                    if pending_error is None:
                        pending_error = exc
            return pending_error

        def terminal(
            exc: OSError, *, held_keys: int | None = None,
            held_buttons: int | None = None,
        ) -> ObserverOutcome:
            pending_error = finish_pending()
            return classify_observer_error(
                pending_error or exc,
                phase=phase,
                exact_device=True,
                retirement_requested=retirement,
                relevant_events=relevant,
                unexpected_events=unexpected,
                held_keys=max(len(self.held_keys), held_keys or 0),
                held_buttons=max(len(self.held_buttons), held_buttons or 0),
            )

        while time.monotonic() < deadline and not all(retired):
            for index, observer in enumerate(self.observers):
                if retired[index]:
                    continue
                try:
                    events = observer.read(min(0.02, max(0.0, deadline - time.monotonic())))
                except OSError as exc:
                    if (retirement and
                            phase is BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED and
                            exc.errno in {errno.ENODEV, errno.ENOENT}):
                        # Retirement is per exact observer. Keep collecting the
                        # other observer until it also retires or the bounded
                        # observation window closes; its fd may still own
                        # queued input that has not reached Python's buffer.
                        retired[index] = True
                        if retirement_error is None:
                            retirement_error = exc
                        continue
                    return terminal(exc)
                aggregate(observer, events)

        # Close the deadline edge explicitly. A different observer may have
        # become readable while the preceding fd consumed the final blocking
        # slice. Each live fd gets a bounded nonblocking drain before pending
        # buffers or held-state queries can authorize aggregate success.
        final_error: OSError | None = None
        for index, observer in enumerate(self.observers):
            if retired[index]:
                continue
            for _ in range(64):
                try:
                    events = observer.read(0)
                except OSError as exc:
                    if (retirement and
                            phase is BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED and
                            exc.errno in {errno.ENODEV, errno.ENOENT}):
                        retired[index] = True
                        if retirement_error is None:
                            retirement_error = exc
                    elif final_error is None:
                        final_error = exc
                    break
                aggregate(observer, events)
                if not events:
                    break
            else:
                if final_error is None:
                    final_error = OSError(errno.EIO, "observer final drain exceeded bound")
        if final_error is not None:
            return terminal(final_error)
        pending_error = finish_pending()
        if pending_error is not None:
            return terminal(pending_error)
        keys = len(self.held_keys)
        buttons = len(self.held_buttons)
        for index, observer in enumerate(self.observers):
            if retired[index]:
                continue
            try:
                held = observer.held_count()
            except OSError as exc:
                if (retirement and
                        phase is BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED and
                        exc.errno in {errno.ENODEV, errno.ENOENT}):
                    retired[index] = True
                    if retirement_error is None:
                        retirement_error = exc
                    continue
                return terminal(exc, held_keys=keys, held_buttons=buttons)
            if index == 0:
                keys = max(keys, held)
            else:
                buttons = max(buttons, held)
        if retirement_error is not None:
            return classify_observer_error(
                retirement_error,
                phase=phase,
                exact_device=True,
                retirement_requested=retirement,
                relevant_events=relevant,
                unexpected_events=unexpected,
                held_keys=keys,
                held_buttons=buttons,
            )
        return observation_complete(
            relevant_events=relevant,
            unexpected_events=unexpected,
            held_keys=keys,
            held_buttons=buttons,
        )

    def close(self) -> None:
        for observer in self.observers:
            observer.close()


def _route_is(value: object, route: str) -> bool:
    return (
        getattr(getattr(value, "desired", None), "value", None) == route
        and getattr(getattr(value, "active", None), "value", None) == route
        and getattr(value, "transition", None) == "stable"
        and bool(getattr(value, "ready", False)) == (route != "none")
    )


def _ble_ready(exposure: object, pairing: object, bonds: object) -> bool:
    connected = [bond for bond in bonds.bonds if bond.connected]
    return all(
        (
            exposure.desired.value == "exposed",
            exposure.observed.value == "connected",
            exposure.stack_ready,
            exposure.connected,
            not exposure.recovery_required,
            exposure.last_error is None,
            pairing.state.value == "idle",
            pairing.connected,
            pairing.pairing_id is None,
            pairing.encrypted,
            pairing.authenticated,
            pairing.bonded,
            pairing.secure_connections,
            pairing.key_size == 16,
            bonds.healthy,
            len(connected) == 1,
            connected[0].our_sec,
            connected[0].peer_sec,
            connected[0].verified,
            connected[0].schema_current,
        )
    )


class Rehearsal:
    def __init__(
        self,
        port: str,
        artifact_identity: object,
        intent: Callable[[str], None],
    ) -> None:
        self.port = port
        self.artifact_identity = artifact_identity
        self.intent = intent
        self.boot_id: str | None = None
        self.sessions: list[dict[str, object]] = []
        self.observers: ExactObservers | None = None
        self.attempt_client: Client | None = None
        self.retired_client: Client | None = None
        self.initial_bonds: tuple[str, ...] = ()
        self.release_results: list[dict[str, str]] = []
        self.all_up_observation: dict[str, object] | None = None
        self.mutations = {
            "sequence_start": 0,
            "keyboard_report": 0,
            "mouse_report": 0,
            "release_all": 0,
            "route_ble": 0,
            "route_none": 0,
            "ble_enable": 0,
            "ble_disable": 0,
        }

    def acquire(self, label: str) -> Client:
        last_error: BaseException | None = None
        for attempt in range(1, 4):
            client: Client | None = None
            try:
                transport = PySerialTransport(self.port, 115200)
                transport.open()
                client = Client(transport, timeout=0.9, max_attempts=1)
                hello = client.connect()
                if self.boot_id is not None and hello.boot_id != self.boot_id:
                    raise QualificationError("device boot identity changed during rehearsal")
                self.sessions.append({"label": label, "attempts": attempt, "fresh": True})
                return client
            except (OSError, SessionLostError) as exc:
                last_error = exc
                if client is not None:
                    client.close()
                time.sleep(0.1)
            except Exception:
                if client is not None:
                    client.close()
                raise
        raise QualificationError("bounded fresh session acquisition failed") from last_error

    def preflight(self, address: str) -> dict[str, object]:
        client = self.acquire("preflight")
        try:
            info = validate_system_info(client.info(), capabilities=client.capabilities)
            identity = compare_firmware_identity(
                self.artifact_identity, client.capabilities, info
            )
            if not identity.match:
                raise QualificationError("runtime firmware identity does not match artifact")
            self.boot_id = client.boot_id
            route = client.hid_route_status()
            if not _route_is(route, "none"):
                raise QualificationError("rehearsal must begin at stable route none")
            exposure = client.ble_exposure_status()
            if exposure.recovery_required or exposure.last_error is not None:
                raise QualificationError("BLE lifecycle is faulted before rehearsal")
            enable_required = exposure.desired.value != "exposed"
        finally:
            client.close()

        if enable_required:
            self.mutations["ble_enable"] += 1
            self.intent("ble.enable")
            client = self.acquire("ble_enable")
            try:
                client.ble_enable()
            finally:
                client.close()

        _connect_target(address, self.intent)
        deadline = time.monotonic() + 12
        client = self.acquire("secure_ready")
        try:
            while True:
                exposure = client.ble_exposure_status()
                pairing = client.ble_pairing_status()
                bonds = client.ble_bond_list()
                if _ble_ready(exposure, pairing, bonds):
                    break
                if time.monotonic() >= deadline:
                    raise QualificationError("strict retained BLE security did not become ready")
                time.sleep(0.2)
            self.initial_bonds = tuple(sorted(bond.bond_id for bond in bonds.bonds))
        finally:
            client.close()

        # Persist the entire cleanup mutation plan before entering the BLE
        # observer critical interval. Capsule fsync latency must not consume a
        # later fresh session or create a gap after the quiet baseline.
        for operation in ("route.change", "cleanup", "route.change", "ble.disable"):
            self.intent(operation)

        self.mutations["route_ble"] += 1
        client = self.acquire("route_setup")
        try:
            response = client.hid_route_set(OutputRouteV2.BLE)
            if not _route_is(response, "ble"):
                raise QualificationError("BLE route request did not return stable ready BLE")
        finally:
            client.close()
        # Route selection intentionally retires setup authority. Start the
        # attempt after that boundary and before workload/baseline observation.
        self.attempt_client = client = self.acquire("attempt")

        self.observers = ExactObservers.discover(address)
        baseline = self.observers.collect(
            0.25, phase=BleCleanupPhase.WORKLOAD_OBSERVATION, retirement=False
        )
        if (
            baseline.status.value != "observation_complete"
            or baseline.relevant_events
            or baseline.unexpected_events
            or baseline.held_keys
            or baseline.held_buttons
        ):
            raise QualificationError("BLE observer baseline is not quiet and all-up")

        # Keep the route/workload session: hello here would invoke takeover
        # safety and invalidate the route continuity this rehearsal measures.
        client.ping()
        if not _route_is(client.hid_route_status(), "ble"):
            raise QualificationError("BLE route lost during observer setup")

        return {
            "runtime_identity": "MATCH",
            "initial_route": "none",
            "ble_security": "strict_ready",
            "bond_count": len(self.initial_bonds),
            "observers": {"exact_target": True, "keyboard": 1, "mouse": 1},
            "quiet_baseline": {"duration_ms": 250, "status": "pass"},
            "route_ble_response": "stable_ready",
        }

    def cleanup(self) -> dict[str, object]:
        if self.observers is None or self.attempt_client is None:
            raise QualificationError("attempt session and observers must be open")

        def release(client: Client) -> object:
            self.mutations["release_all"] += 1
            result = client.release_all()
            self.release_results.append(asdict(result))
            return result

        def all_up() -> ObserverOutcome:
            result = self.observers.collect(
                0.15,
                phase=BleCleanupPhase.PRE_RETIREMENT_CLEANUP,
                retirement=False,
            )
            self.all_up_observation = asdict(result)
            return result

        def retire(client: Client) -> object:
            self.mutations["route_none"] += 1
            result = client.hid_route_set(OutputRouteV2.NONE)
            if getattr(getattr(result, "desired", None), "value", None) != "none":
                raise QualificationError("route retirement did not accept none intent")
            return result

        def terminal(client: Client) -> object:
            self.mutations["ble_disable"] += 1
            return client.ble_disable()

        def final(client: Client) -> FinalBleControlState:
            deadline = time.monotonic() + 5
            while True:
                route = client.hid_route_status()
                exposure = client.ble_exposure_status()
                if (
                    _route_is(route, "none")
                    and exposure.desired.value == "hidden"
                    and exposure.observed.value in {"idle", "uninitialized"}
                    and not exposure.connected
                    and not exposure.advertising
                    and not exposure.recovery_required
                    and exposure.last_error is None
                ):
                    bonds = client.ble_bond_list()
                    after = tuple(sorted(bond.bond_id for bond in bonds.bonds))
                    if after != self.initial_bonds or not bonds.healthy:
                        raise QualificationError("BLE bond inventory changed during rehearsal")
                    return FinalBleControlState(
                        route_none=True,
                        sequence_active=False,
                        held_state="ALL_UP",
                        ble_connected=False,
                        ble_terminal="hidden_idle_disconnected",
                    )
                if time.monotonic() >= deadline:
                    raise QualificationError("final BLE control state did not converge")
                time.sleep(0.1)

        def acquire_retired() -> Client:
            self.attempt_client.close()
            self.attempt_client = None
            self.retired_client = self.acquire("post_retirement")
            return self.retired_client

        return run_ble_cleanup(
            record_mutation_intent=lambda _operation: None,
            attempt_session=self.attempt_client,
            session_identity=lambda client: (client.boot_id, client.session),
            keep_session_alive=lambda client: client.ping(),
            release_all=release,
            observe_all_up=all_up,
            observe_quiet_tail=lambda: self.observers.collect(
                0.25, phase=BleCleanupPhase.ALL_UP_PROVEN, retirement=False
            ),
            request_retirement=retire,
            observe_retirement=lambda: self.observers.collect(
                1.0,
                phase=BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED,
                retirement=True,
            ),
            acquire_retired_session=acquire_retired,
            request_ble_terminal=terminal,
            read_final_state=final,
            required_ble_terminal="hidden_idle_disconnected",
        )

    def emergency_safe_state(self) -> list[dict[str, str]]:
        # Separate recovery may acquire new authority; it can never repair
        # continuity or convert the failed rehearsal result into PASS.
        if self.attempt_client is not None:
            self.attempt_client.close()
            self.attempt_client = None
        if self.retired_client is not None:
            self.retired_client.close()
            self.retired_client = None
        results: list[dict[str, str]] = []
        for name, action in (
            ("release_all", lambda client: client.release_all()),
            ("route_none", lambda client: client.hid_route_set(OutputRouteV2.NONE)),
            ("ble_disable", lambda client: client.ble_disable()),
        ):
            client: Client | None = None
            try:
                operation = {
                    "release_all": "cleanup",
                    "route_none": "route.change",
                    "ble_disable": "ble.disable",
                }[name]
                self.intent(operation)
                client = self.acquire("recovery_" + name)
                action(client)
                results.append({"name": name, "status": "pass"})
            except Exception as exc:
                results.append(
                    {"name": name, "status": "fail", "classification": type(exc).__name__}
                )
            finally:
                if client is not None:
                    client.close()
        return results

    def close(self) -> None:
        if self.attempt_client is not None:
            self.attempt_client.close()
            self.attempt_client = None
        if self.retired_client is not None:
            self.retired_client.close()
            self.retired_client = None
        if self.observers is not None:
            self.observers.close()

    def partial_cleanup_evidence(self) -> dict[str, object]:
        return {
            "release_results": self.release_results,
            "all_up_observation": self.all_up_observation,
        }


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run BLE cleanup-only physical rehearsal")
    parser.add_argument("--hardware", action="store_true")
    parser.add_argument("--cleanup-rehearsal-only", action="store_true")
    parser.add_argument("--not-qualification", action="store_true")
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--expected-source", required=True)
    parser.add_argument("--expected-artifact-sha256", required=True)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--serial")
    parser.add_argument("--capsule-root", type=Path)
    parser.add_argument("--capsule-run-id")
    parser.add_argument("--capsule-infra", type=Path)
    return parser


def _intent_recorder(args: argparse.Namespace) -> Callable[[str], None]:
    values = (args.capsule_root, args.capsule_run_id, args.capsule_infra)
    if not any(values):
        return lambda _operation: None
    if not all(values):
        raise QualificationError("capsule evidence arguments must be supplied together")
    sys.path.insert(0, str(args.capsule_infra))
    from run_capsule import append_event

    return lambda operation: append_event(
        args.capsule_root, args.capsule_run_id, "SIDE_EFFECT_INVOKED", operation
    )


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if not (args.hardware and args.cleanup_rehearsal_only and args.not_qualification):
        print("Refusing hardware access without all cleanup-only acknowledgements", file=sys.stderr)
        return 2

    started = time.monotonic()
    evidence: dict[str, object] = {
        "schema": 2,
        "classification": CLASSIFICATION,
        "result": "FAIL",
        "qualification_attempt_consumed": False,
        "workload": {
            "sequence_start": 0,
            "keyboard_report": 0,
            "mouse_report": 0,
        },
        "authority": {
            "product_source": args.expected_source,
            "archive_sha256": args.expected_artifact_sha256,
            "build_profile": BUILD_PROFILE,
        },
    }
    rehearsal: Rehearsal | None = None
    try:
        if not re.fullmatch(r"[0-9a-f]{40}", args.expected_source):
            raise QualificationError("expected source must be an exact commit")
        if not re.fullmatch(r"[0-9a-f]{64}", args.expected_artifact_sha256):
            raise QualificationError("expected archive SHA256 is required")
        if _sha256(args.artifact) != args.expected_artifact_sha256:
            raise QualificationError("artifact archive digest mismatch")
        intent = _intent_recorder(args)
        headless = _headless()
        evidence["headless"] = headless
        if not headless["safe"]:
            raise QualificationError("rp-test must remain headless")
        adapter = _adapter_state()
        evidence["adapter"] = adapter
        if not (
            adapter["adapter_count"] == 1
            and adapter["readable"]
            and adapter["powered"]
            and not adapter["pairable"]
            and not adapter["discoverable"]
        ):
            raise QualificationError("BlueZ adapter precondition is not safe")
        address, bluez = _paired_target()
        evidence["bluez_precondition"] = bluez
        port = _serial_port(args.serial)
        with stage_and_inspect_firmware_bundle(args.artifact) as bundle:
            identity = bundle.artifact_identity
            if not (
                identity.source_revision == args.expected_source
                and identity.build_profile == BUILD_PROFILE
            ):
                raise QualificationError("artifact identity is not the expected candidate")
            application = bundle.staged_root / "s3_hidbot_passive_usb_hid.bin"
            executable = bundle.staged_root / "s3_hidbot_passive_usb_hid.elf"
            evidence["authority"].update({
                "application_sha256": _sha256(application),
                "elf_sha256": _sha256(executable),
            })
            rehearsal = Rehearsal(port, identity, intent)
            evidence["preflight"] = rehearsal.preflight(address)
            evidence["cleanup"] = rehearsal.cleanup()
            evidence["sessions"] = rehearsal.sessions
            evidence["mutations"] = rehearsal.mutations
            if any(rehearsal.mutations[name] for name in ("sequence_start", "keyboard_report", "mouse_report")):
                raise QualificationError("cleanup rehearsal attempted a forbidden workload")
            evidence["result"] = "PASS"
    except Exception as exc:
        causes: list[str] = []
        current: BaseException | None = exc
        while current is not None and len(causes) < 4:
            causes.append(type(current).__name__)
            current = current.__cause__
        evidence["failure"] = {
            "classification": type(exc).__name__,
            "cause_chain": causes,
            "message": str(exc),
        }
        if rehearsal is not None:
            evidence["partial_cleanup"] = rehearsal.partial_cleanup_evidence()
            evidence["recovery"] = rehearsal.emergency_safe_state()
            evidence["sessions"] = rehearsal.sessions
            evidence["mutations"] = rehearsal.mutations
    finally:
        if rehearsal is not None:
            rehearsal.close()
        evidence["duration_ms"] = round((time.monotonic() - started) * 1000)

    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":")) + "\n"
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(
        args.evidence,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0),
        0o600,
    )
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(encoded)
    print("BLE_CLEANUP_REHEARSAL=" + str(evidence["result"]))
    print("CLASSIFICATION=" + CLASSIFICATION)
    return 0 if evidence["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
