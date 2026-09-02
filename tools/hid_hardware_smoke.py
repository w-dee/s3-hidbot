#!/usr/bin/env python3
"""Read-only Linux HID discovery and opt-in keyboard/mouse smoke support.

The default mode deliberately stops after discovering and opening the two
evdev sources.  The optional ``--hardware --keyboard`` and
``--hardware --mouse`` modes are separately gated physical smoke paths.  They
are never entered without the explicit ``--hardware`` opt-in.
"""

from __future__ import annotations

import argparse
import errno
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
import re
import selectors
import struct
import sys
import time
from typing import Any, Callable, Iterable, TextIO


DEFAULT_VID = 0x303A
DEFAULT_PID = 0x4008
DEFAULT_PRODUCT = "s3-hidbot"
DEFAULT_KEYBOARD_INTERFACE = 0
DEFAULT_MOUSE_INTERFACE = 1
KEY_F24 = 194
F24_USAGE = 0x73
REL_X = 0
EV_KEY = 0x01
EV_REL = 0x02
EV_SYN = 0x00
EV_MSC = 0x04
MSC_SCAN = 0x04
SYN_REPORT = 0x00
SYN_DROPPED = 0x03
REL_Y = 0x01
REL_HWHEEL = 0x06
REL_WHEEL = 0x08
EVENT_NAME = re.compile(r"event[0-9]+\Z")
DEFAULT_BAUD = 115200
DEFAULT_EVENT_TIMEOUT = 2.0
DEFAULT_QUIET_TAIL = 0.15
MAX_EVENT_EVIDENCE = 32
# The UP request follows the DOWN observation immediately.  Two queued F24
# repeats absorb a short release race without accepting a repeat storm.
MAX_ALLOWED_F24_REPEATS = 2


class DiscoveryError(RuntimeError):
    """The requested HID input topology was not uniquely identified."""


class ObserverError(RuntimeError):
    """An event stream could not be decoded as complete input records."""


class KeyboardSmokeError(RuntimeError):
    """A bounded F24 smoke phase failed with a reportable exit code."""

    def __init__(self, message: str, *, exit_code: int = 5) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class MouseSmokeError(RuntimeError):
    """A bounded relative-mouse smoke phase failed."""

    def __init__(self, message: str, *, exit_code: int = 5) -> None:
        super().__init__(message)
        self.exit_code = exit_code


@dataclass(frozen=True)
class DeviceIdentity:
    vid: int = DEFAULT_VID
    pid: int = DEFAULT_PID
    product: str = DEFAULT_PRODUCT


@dataclass(frozen=True)
class InputEvent:
    seconds: int
    microseconds: int
    event_type: int
    code: int
    value: int


class _EventEvidence:
    """Keep a small, structured trace without allowing unbounded output."""

    def __init__(self, limit: int = MAX_EVENT_EVIDENCE) -> None:
        self.limit = limit
        self.events: list[dict[str, object]] = []
        self.truncated = False
        self.allowed_repeat_count = 0
        self.unexpected_event: dict[str, object] | None = None
        self.repeat_limit_event: dict[str, object] | None = None

    def record(
        self,
        *,
        phase: str,
        batch: int,
        index: int,
        event: InputEvent,
        classification: str,
    ) -> None:
        if classification in {"allowed_repeat", "repeat_limit_exceeded"}:
            self.allowed_repeat_count += 1
        entry = {
            "phase": phase,
            "batch": batch,
            "index": index,
            "type": event.event_type,
            "code": event.code,
            "value": event.value,
            "timestamp_sec": event.seconds,
            "timestamp_usec": event.microseconds,
            "classification": classification,
        }
        if classification == "unexpected" and self.unexpected_event is None:
            self.unexpected_event = entry
        if (
            classification == "repeat_limit_exceeded"
            and self.repeat_limit_event is None
        ):
            self.repeat_limit_event = entry
        if len(self.events) < self.limit:
            self.events.append(entry)
        else:
            self.truncated = True

    def update(self, evidence: dict[str, object]) -> None:
        evidence["event_evidence"] = list(self.events)
        evidence["event_evidence_truncated"] = self.truncated
        evidence["allowed_repeat_count"] = self.allowed_repeat_count
        evidence["repeat_limit_exceeded"] = self.repeat_limit_event is not None
        if self.unexpected_event is not None:
            evidence["unexpected_event"] = dict(self.unexpected_event)
        if self.repeat_limit_event is not None:
            evidence["repeat_limit_event"] = dict(self.repeat_limit_event)


class _MouseEventEvidence:
    """Keep bounded, structured evidence for the relative-mouse path."""

    def __init__(self, limit: int = MAX_EVENT_EVIDENCE) -> None:
        self.limit = limit
        self.events: list[dict[str, object]] = []
        self.truncated = False
        self.unexpected_event: dict[str, object] | None = None

    def record(
        self,
        *,
        phase: str,
        batch: int,
        index: int,
        event: InputEvent,
        classification: str,
    ) -> None:
        entry = {
            "phase": phase,
            "batch": batch,
            "index": index,
            "type": event.event_type,
            "code": event.code,
            "value": event.value,
            "timestamp_sec": event.seconds,
            "timestamp_usec": event.microseconds,
            "classification": classification,
        }
        if classification == "unexpected" and self.unexpected_event is None:
            self.unexpected_event = entry
        if len(self.events) < self.limit:
            self.events.append(entry)
        else:
            self.truncated = True

    def update(self, evidence: dict[str, object]) -> None:
        evidence["event_evidence"] = list(self.events)
        evidence["event_evidence_truncated"] = self.truncated
        if self.unexpected_event is not None:
            evidence["unexpected_event"] = dict(self.unexpected_event)


def is_syn_dropped(event: InputEvent) -> bool:
    return event.event_type == EV_SYN and event.code == SYN_DROPPED


def is_f24_event(event: InputEvent, value: int | None = None) -> bool:
    return (
        event.event_type == EV_KEY
        and event.code == KEY_F24
        and (value is None or event.value == value)
    )


def is_rel_x_event(event: InputEvent, value: int | None = None) -> bool:
    return (
        event.event_type == EV_REL
        and event.code == REL_X
        and (value is None or event.value == value)
    )


@dataclass(frozen=True)
class InputCandidate:
    path: Path
    name: str
    vid: int
    pid: int
    product: str
    interface_number: int
    capabilities: tuple[str, ...]

    def as_dict(self) -> dict[str, object]:
        return {
            "event": self.path.name,
            # Keep evidence generic even when a test/lab override uses a
            # non-standard root; never echo a machine-local absolute path.
            "path": f"/dev/input/{self.path.name}",
            "name": self.name,
            "vid": f"0x{self.vid:04x}",
            "pid": f"0x{self.pid:04x}",
            "product": self.product,
            "interface": self.interface_number,
            "capabilities": list(self.capabilities),
        }


class InputEventDecoder:
    """Decode native Linux ``struct input_event`` records incrementally."""

    # timeval is two native longs.  This is 16 bytes on 32-bit Linux and
    # 24 bytes on the 64-bit Linux hosts supported by this runner.
    _record = struct.Struct("@llHHi")

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[InputEvent]:
        self._buffer.extend(data)
        records: list[InputEvent] = []
        size = self._record.size
        while len(self._buffer) >= size:
            seconds, microseconds, event_type, code, value = self._record.unpack(
                self._buffer[:size]
            )
            del self._buffer[:size]
            records.append(
                InputEvent(seconds, microseconds, event_type, code, value)
            )
        return records

    @property
    def pending_bytes(self) -> int:
        return len(self._buffer)


class ReadOnlyEventObserver:
    """An O_RDONLY, non-blocking observer for one evdev event node."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.fd: int | None = None
        self.decoder = InputEventDecoder()
        self.saw_syn_dropped = False

    def open(self) -> None:
        if self.fd is not None:
            return
        flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)
        self.fd = os.open(self.path, flags)

    def drain(self) -> int:
        if self.fd is None:
            raise RuntimeError("observer is not open")
        count = 0
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    break
                raise
            if not data:
                break
            events = self.decoder.feed(data)
            self.saw_syn_dropped |= any(is_syn_dropped(event) for event in events)
            count += len(events)
        if self.saw_syn_dropped:
            raise ObserverError("SYN_DROPPED made input evidence incomplete")
        if self.decoder.pending_bytes:
            raise ObserverError("partial input_event record at end of drain")
        return count

    def wait_events(self, timeout: float) -> list[InputEvent]:
        """Read a bounded batch without blocking beyond ``timeout``."""
        if self.fd is None:
            raise RuntimeError("observer is not open")
        if timeout < 0:
            raise ValueError("timeout must be non-negative")
        with selectors.DefaultSelector() as selector:
            selector.register(self.fd, selectors.EVENT_READ)
            if not selector.select(timeout):
                return []
        events: list[InputEvent] = []
        while True:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                break
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    break
                raise
            if not data:
                break
            decoded = self.decoder.feed(data)
            self.saw_syn_dropped |= any(is_syn_dropped(event) for event in decoded)
            events.extend(decoded)
        if self.saw_syn_dropped:
            raise ObserverError("SYN_DROPPED made input evidence incomplete")
        if self.decoder.pending_bytes:
            raise ObserverError("partial input_event record at end of wait")
        return events

    def close(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return None


def _read_ancestor_file(start: Path, filename: str) -> str | None:
    for ancestor in (start, *start.parents):
        value = _read_text(ancestor / filename)
        if value is not None:
            return value
    return None


def _parse_hex(value: str, description: str) -> int:
    try:
        return int(value.strip(), 16)
    except ValueError as exc:
        raise DiscoveryError(f"invalid {description}: {value!r}") from exc


def _parse_interface(value: str) -> int:
    try:
        return int(value.strip(), 0)
    except ValueError:
        return _parse_hex(value, "bInterfaceNumber")


def _capability_bits(path: Path) -> int | None:
    text = _read_text(path)
    if text is None or not text:
        return None
    return parse_capability_bitset(text)


def parse_capability_bitset(text: str) -> int | None:
    """Parse Linux's whitespace-separated, most-significant-first bitmap."""
    words = text.split()
    try:
        return int("".join(words), 16)
    except ValueError:
        return None


def _has_capability(event_class: Path, resolved_event: Path, group: str, bit: int) -> bool:
    paths = (
        event_class / "device" / "capabilities" / group,
        resolved_event.parent / "capabilities" / group,
        resolved_event / "device" / "capabilities" / group,
    )
    for path in paths:
        bits = _capability_bits(path)
        if bits is not None:
            return bool(bits & (1 << bit))
    return False


def _candidate(
    event_path: Path,
    sysfs_input: Path,
    identity: DeviceIdentity,
    interface_number: int,
    required_group: str,
    required_bit: int,
) -> InputCandidate | None:
    if not EVENT_NAME.fullmatch(event_path.name) or event_path.is_symlink():
        return None
    event_class = sysfs_input / event_path.name
    if not event_class.exists():
        return None
    try:
        resolved_event = event_class.resolve(strict=True)
    except OSError:
        return None

    raw_vid = _read_ancestor_file(resolved_event, "idVendor")
    raw_pid = _read_ancestor_file(resolved_event, "idProduct")
    product = _read_ancestor_file(resolved_event, "product")
    raw_interface = _read_ancestor_file(resolved_event, "bInterfaceNumber")
    if None in (raw_vid, raw_pid, product, raw_interface):
        return None
    try:
        vid = _parse_hex(raw_vid or "", "idVendor")
        pid = _parse_hex(raw_pid or "", "idProduct")
        found_interface = _parse_interface(raw_interface or "")
    except DiscoveryError:
        return None
    if (vid, pid, product) != (identity.vid, identity.pid, identity.product):
        return None
    if found_interface != interface_number:
        return None
    if not _has_capability(event_class, resolved_event, required_group, required_bit):
        return None

    name = _read_text(event_class / "device" / "name")
    if name is None:
        name = _read_text(resolved_event.parent / "name") or "unknown"
    capabilities = [required_group]
    if required_group != "rel" and _has_capability(event_class, resolved_event, "rel", REL_X):
        capabilities.append("rel")
    if required_group != "key" and _has_capability(event_class, resolved_event, "key", KEY_F24):
        capabilities.append("key")
    return InputCandidate(
        path=event_path,
        name=name,
        vid=vid,
        pid=pid,
        product=product,
        interface_number=found_interface,
        capabilities=tuple(capabilities),
    )


def _event_paths(dev_input: Path, override: str | None) -> list[Path]:
    if override is not None:
        path = Path(override)
        if not path.is_absolute():
            path = dev_input / path
        try:
            if path.parent.resolve() != dev_input.resolve():
                raise DiscoveryError("event override must be directly under /dev/input")
        except OSError as exc:
            raise DiscoveryError(f"invalid event override: {path}") from exc
        return [path]
    return sorted(
        (path for path in dev_input.glob("event*") if EVENT_NAME.fullmatch(path.name)),
        key=lambda path: path.name,
    )


def _select_one(
    role: str,
    paths: Iterable[Path],
    sysfs_input: Path,
    identity: DeviceIdentity,
    interface_number: int,
    required_group: str,
    required_bit: int,
) -> InputCandidate:
    matches = [
        found
        for path in paths
        if (found := _candidate(
            path,
            sysfs_input,
            identity,
            interface_number,
            required_group,
            required_bit,
        ))
        is not None
    ]
    if len(matches) != 1:
        count = len(matches)
        raise DiscoveryError(f"expected exactly one {role} event device, found {count}")
    return matches[0]


def discover_devices(
    *,
    dev_input_root: Path = Path("/dev/input"),
    sysfs_root: Path = Path("/sys"),
    identity: DeviceIdentity = DeviceIdentity(),
    keyboard_interface: int = DEFAULT_KEYBOARD_INTERFACE,
    mouse_interface: int = DEFAULT_MOUSE_INTERFACE,
    keyboard_event: str | None = None,
    mouse_event: str | None = None,
) -> tuple[InputCandidate, InputCandidate]:
    """Find exactly one validated keyboard and mouse event source."""

    sysfs_input = sysfs_root / "class" / "input"
    keyboard = _select_one(
        "keyboard",
        _event_paths(dev_input_root, keyboard_event),
        sysfs_input,
        identity,
        keyboard_interface,
        "key",
        KEY_F24,
    )
    mouse = _select_one(
        "mouse",
        _event_paths(dev_input_root, mouse_event),
        sysfs_input,
        identity,
        mouse_interface,
        "rel",
        REL_X,
    )
    if keyboard.path == mouse.path:
        raise DiscoveryError("keyboard and mouse event devices must be distinct")
    return keyboard, mouse


def discover_keyboard(
    *,
    dev_input_root: Path = Path("/dev/input"),
    sysfs_root: Path = Path("/sys"),
    identity: DeviceIdentity = DeviceIdentity(),
    keyboard_interface: int = DEFAULT_KEYBOARD_INTERFACE,
    keyboard_event: str | None = None,
) -> InputCandidate:
    """Find exactly one validated F24-capable keyboard event source."""

    return _select_one(
        "keyboard",
        _event_paths(dev_input_root, keyboard_event),
        sysfs_root / "class" / "input",
        identity,
        keyboard_interface,
        "key",
        KEY_F24,
    )


def discover_mouse(
    *,
    dev_input_root: Path = Path("/dev/input"),
    sysfs_root: Path = Path("/sys"),
    identity: DeviceIdentity = DeviceIdentity(),
    mouse_interface: int = DEFAULT_MOUSE_INTERFACE,
    mouse_event: str | None = None,
) -> InputCandidate:
    """Find exactly one REL_X-capable mouse event source."""

    return _select_one(
        "mouse",
        _event_paths(dev_input_root, mouse_event),
        sysfs_root / "class" / "input",
        identity,
        mouse_interface,
        "rel",
        REL_X,
    )


def _report_state(result: object) -> str:
    """Read the typed host result without duplicating the protocol model."""

    if isinstance(result, dict):
        state = result.get("state")
    else:
        state = getattr(result, "state", None)
    return state if isinstance(state, str) else "unknown"


def _release_result(
    result: object,
    *,
    error_type: type[RuntimeError] = KeyboardSmokeError,
) -> dict[str, str]:
    """Convert the existing typed release result to bounded evidence."""

    if isinstance(result, dict):
        keyboard = result.get("keyboard")
        mouse = result.get("mouse")
    else:
        keyboard = getattr(result, "keyboard", None)
        mouse = getattr(result, "mouse", None)
    if keyboard not in {"already_up", "submitted"} or mouse not in {
        "already_up",
        "submitted",
    }:
        raise error_type("release_all returned an invalid result", exit_code=3)
    return {"keyboard": keyboard, "mouse": mouse}


def _wait_for_f24(
    observer: Any,
    expected_value: int,
    timeout: float,
    *,
    clock: Callable[[], float],
    phase: str = "f24_wait",
    batch_number: int = 0,
    evidence: _EventEvidence | None = None,
    observation: dict[str, object] | None = None,
) -> tuple[float, int]:
    """Wait for one F24 edge while applying the phase-specific policy.

    The down phase is strict.  The up phase may tolerate only F24 value 2
    before the required value 0.  The complete batch is evaluated before a
    failure is raised so evidence can report a release followed by an
    unexpected event accurately.
    """

    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("event timeout must be positive")
    started = clock()
    events = observer.wait_events(timeout)
    matched = False
    observed_repeats = 0
    failure: tuple[str, InputEvent] | None = None
    for index, event in enumerate(events):
        classification = "ignored"
        if is_syn_dropped(event):
            classification = "syn_dropped"
            if failure is None:
                failure = ("SYN_DROPPED made F24 evidence incomplete", event)
        elif event.event_type == EV_SYN:
            classification = "ignored_syn"
        elif event.event_type == EV_KEY:
            if (
                expected_value == 0
                and not matched
                and is_f24_event(event, 2)
            ):
                observed_repeats += 1
                if observed_repeats <= MAX_ALLOWED_F24_REPEATS:
                    classification = "allowed_repeat"
                else:
                    classification = "repeat_limit_exceeded"
                    if failure is None:
                        failure = (
                            "F24 autorepeat limit exceeded before release",
                            event,
                        )
            elif is_f24_event(event, expected_value) and not matched:
                matched = True
                classification = (
                    "expected_down" if expected_value == 1 else "expected_up"
                )
            else:
                classification = "unexpected"
                if failure is None:
                    failure = (
                        "unexpected keyboard key event during F24 smoke",
                        event,
                    )
        if evidence is not None:
            evidence.record(
                phase=phase,
                batch=batch_number,
                index=index,
                event=event,
                classification=classification,
            )
    elapsed = max(0.0, (clock() - started) * 1000.0)
    if observation is not None:
        observation["matched"] = matched
        observation["latency_ms"] = elapsed if matched else None
        observation["allowed_repeat_count"] = observed_repeats
    if failure is not None:
        raise KeyboardSmokeError(failure[0], exit_code=5)
    if matched:
        return elapsed, observed_repeats
    raise KeyboardSmokeError(
        f"timed out waiting for F24 value {expected_value}", exit_code=5
    )


def _quiet_tail(
    observer: Any,
    timeout: float,
    *,
    phase: str = "up_tail",
    batch_number: int = 0,
    evidence: _EventEvidence | None = None,
) -> None:
    """Catch duplicate or unrelated keyboard events in a short bounded tail."""

    if timeout <= 0:
        return
    failure: tuple[str, InputEvent] | None = None
    for index, event in enumerate(observer.wait_events(timeout)):
        classification = "ignored"
        if is_syn_dropped(event):
            classification = "syn_dropped"
            if failure is None:
                failure = (
                    "SYN_DROPPED made F24 quiet-tail evidence incomplete",
                    event,
                )
        elif event.event_type == EV_SYN:
            classification = "ignored_syn"
        elif event.event_type == EV_KEY:
            classification = "unexpected"
            if failure is None:
                failure = (
                    "unexpected or repeated keyboard key event after F24 report",
                    event,
                )
        if evidence is not None:
            evidence.record(
                phase=phase,
                batch=batch_number,
                index=index,
                event=event,
                classification=classification,
            )
    if failure is not None:
        raise KeyboardSmokeError(failure[0], exit_code=5)


def run_keyboard_smoke(
    candidate: InputCandidate,
    *,
    serial_port: str,
    baudrate: int = DEFAULT_BAUD,
    event_timeout: float = DEFAULT_EVENT_TIMEOUT,
    quiet_tail: float = DEFAULT_QUIET_TAIL,
    observer_factory: Callable[[Path], Any] = ReadOnlyEventObserver,
    transport_factory: Callable[[str, int, float], Any],
    client_factory: Callable[[Any, float], Any],
    clock: Callable[[], float] = time.monotonic,
) -> tuple[int, dict[str, object]]:
    """Run one bounded F24 down/up transaction using injected dependencies.

    The injected seams keep all tests off physical `/dev/input` and serial
    devices while the production path reuses the existing host transport and
    ``Client`` APIs. A successful run explicitly selects USB, establishes a
    fresh session, performs one down and one explicit all-up keyboard report,
    then runs ``release_all`` and returns the route to none before close.
    """

    if not serial_port or baudrate <= 0:
        raise ValueError("serial port and positive baudrate are required")
    if (
        not math.isfinite(event_timeout)
        or event_timeout <= 0
        or not math.isfinite(quiet_tail)
        or quiet_tail < 0
    ):
        raise ValueError("event timeout must be positive and quiet tail non-negative")
    started = clock()
    evidence: dict[str, object] = {
        "status": "fail",
        "hardware_opt_in": True,
        "mode": "keyboard",
        "observer_state": "not_open",
        "serial_accessed": False,
        "keyboard": candidate.as_dict(),
        "f24_usage": f"0x{F24_USAGE:02x}",
        "down_submit": None,
        "down_observed": False,
        "down_latency_ms": None,
        "up_submit": None,
        "up_observed": False,
        "up_latency_ms": None,
        "cleanup_attempted": False,
        "cleanup_result": None,
        "route_cleanup": None,
    }
    event_evidence = _EventEvidence()
    event_evidence.update(evidence)
    observer = observer_factory(candidate.path)
    transport: Any | None = None
    client: Any | None = None
    session_started = False
    route_selected = False
    down_accepted = False
    down_observed = False
    cleanup_error: Exception | None = None
    primary_error: KeyboardSmokeError | None = None
    batch_number = 0

    def next_batch() -> int:
        nonlocal batch_number
        current = batch_number
        batch_number += 1
        return current

    def apply_observation(phase: str, observation: dict[str, object]) -> None:
        if phase == "down_wait":
            evidence["down_observed"] = bool(observation.get("matched"))
            evidence["down_latency_ms"] = observation.get("latency_ms")
        elif phase == "up_wait":
            evidence["up_observed"] = bool(observation.get("matched"))
            evidence["up_latency_ms"] = observation.get("latency_ms")

    try:
        observer.open()
        evidence["observer_state"] = "open"
        evidence["initial_drain_events"] = observer.drain()
        evidence["observer_state"] = "ready"

        # This is deliberately after observer readiness: no serial or Client
        # side effect happens until the selected F24 source is validated.
        transport = transport_factory(serial_port, baudrate, event_timeout)
        evidence["serial_accessed"] = True
        transport.open()
        client = client_factory(transport, event_timeout)
        client.connect()
        session_started = True
        client.hid_route_set("usb")
        route_selected = True
        client.connect()

        down_result = client.keyboard_report(modifiers=0, keys=[F24_USAGE])
        down_state = _report_state(down_result)
        evidence["down_submit"] = down_state
        if down_state != "submitted":
            raise KeyboardSmokeError(
                "F24 down was not freshly submitted", exit_code=5
            )
        down_accepted = True
        down_observation: dict[str, object] = {}
        try:
            down_latency, _ = _wait_for_f24(
                observer,
                1,
                event_timeout,
                clock=clock,
                phase="down_wait",
                batch_number=next_batch(),
                evidence=event_evidence,
                observation=down_observation,
            )
        except KeyboardSmokeError:
            apply_observation("down_wait", down_observation)
            raise
        down_observed = True
        apply_observation(
            "down_wait",
            {"matched": True, "latency_ms": down_latency},
        )

        up_result = client.keyboard_report(modifiers=0, keys=[])
        up_state = _report_state(up_result)
        evidence["up_submit"] = up_state
        if up_state != "submitted":
            raise KeyboardSmokeError(
                "F24 up was not freshly submitted", exit_code=5
            )
        up_observation: dict[str, object] = {}
        try:
            up_latency, _ = _wait_for_f24(
                observer,
                0,
                event_timeout,
                clock=clock,
                phase="up_wait",
                batch_number=next_batch(),
                evidence=event_evidence,
                observation=up_observation,
            )
        except KeyboardSmokeError:
            apply_observation("up_wait", up_observation)
            raise
        apply_observation(
            "up_wait",
            {"matched": True, "latency_ms": up_latency},
        )
        _quiet_tail(
            observer,
            quiet_tail,
            phase="up_tail",
            batch_number=next_batch(),
            evidence=event_evidence,
        )

        evidence["cleanup_attempted"] = True
        try:
            evidence["cleanup_result"] = _release_result(client.release_all())
            evidence["route_cleanup"] = client.hid_route_set("none").active.value
            route_selected = False
        except Exception as exc:
            # The final release_all is safety cleanup, so a failure here is a
            # distinct cleanup-only result rather than a primary report error.
            cleanup_error = exc
        evidence["status"] = "pass"
    except KeyboardSmokeError as exc:
        primary_error = exc
    except ObserverError as exc:
        primary_error = KeyboardSmokeError(str(exc), exit_code=5)
    except (OSError, PermissionError) as exc:
        primary_error = KeyboardSmokeError(
            f"keyboard smoke I/O failed: {exc}", exit_code=4
        )
    except Exception as exc:
        # Host Client/transport exceptions are control-plane failures.  Keep
        # the original message while mapping them to the bounded smoke code.
        primary_error = KeyboardSmokeError(
            f"keyboard smoke control operation failed: {exc}", exit_code=3
        )
    finally:
        if session_started and client is not None:
            # If a down report was accepted but no matching down event arrived,
            # make one bounded explicit-up attempt before the final safety call.
            if down_accepted and not down_observed:
                try:
                    client.keyboard_report(modifiers=0, keys=[])
                except Exception:
                    pass
            if not bool(evidence["cleanup_attempted"]):
                evidence["cleanup_attempted"] = True
                try:
                    evidence["cleanup_result"] = _release_result(client.release_all())
                except Exception as exc:
                    cleanup_error = exc
            if route_selected:
                try:
                    evidence["route_cleanup"] = client.hid_route_set("none").active.value
                    route_selected = False
                except Exception as exc:
                    if cleanup_error is None:
                        cleanup_error = exc
        if client is not None:
            try:
                client.close()
            except Exception as exc:
                if primary_error is None and cleanup_error is None:
                    cleanup_error = exc
        elif transport is not None:
            try:
                transport.close()
            except Exception as exc:
                if primary_error is None and cleanup_error is None:
                    cleanup_error = exc
        try:
            observer.close()
        except Exception as exc:
            if primary_error is None and cleanup_error is None:
                cleanup_error = exc

    evidence["total_duration_ms"] = max(0.0, (clock() - started) * 1000.0)
    event_evidence.update(evidence)
    if primary_error is not None:
        evidence["error"] = str(primary_error)
        if cleanup_error is not None:
            evidence["cleanup_error"] = str(cleanup_error)
        return primary_error.exit_code, evidence
    if cleanup_error is not None:
        evidence["status"] = "fail"
        evidence["cleanup_error"] = str(cleanup_error)
        return 6, evidence
    return 0, evidence


def _mouse_event_classification(
    event: InputEvent,
    *,
    movement_observed: bool,
    packet_complete: bool,
) -> str:
    """Classify one event for the strict REL_X target packet."""

    if event.event_type == EV_SYN:
        if event.code == SYN_DROPPED:
            return "syn_dropped"
        if event.code == SYN_REPORT:
            return "packet_boundary"
        return "unexpected"
    if event.event_type == EV_MSC and event.code == MSC_SCAN:
        return "metadata" if not packet_complete else "unexpected"
    if event.event_type == EV_REL and not packet_complete:
        if not movement_observed and event.code == REL_X and event.value == 1:
            return "expected_rel_x"
        return "unexpected"
    return "unexpected"


def _wait_for_mouse_movement(
    observer: Any,
    timeout: float,
    *,
    clock: Callable[[], float],
    evidence: _MouseEventEvidence,
    observation: dict[str, object] | None = None,
    batch_number: int = 0,
) -> tuple[float, int]:
    """Wait for one complete logical REL_X packet.

    ``wait_events`` calls are allowed to split a packet.  A REL_X event is
    evidence that movement occurred, but it is not a complete packet until a
    later SYN_REPORT is observed.  Every event in a returned batch is still
    evaluated so trailing unexpected records cannot be hidden.
    """

    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("event timeout must be positive")
    started = clock()
    movement_observed = False
    packet_complete = False
    failure: str | None = None
    polls = 0
    while not packet_complete:
        elapsed = max(0.0, clock() - started)
        remaining = timeout - elapsed
        if remaining <= 0:
            break
        events = observer.wait_events(remaining)
        current_batch = batch_number + polls
        polls += 1
        if not events:
            break
        for index, event in enumerate(events):
            classification = _mouse_event_classification(
                event,
                movement_observed=movement_observed,
                packet_complete=packet_complete,
            )
            if classification == "expected_rel_x":
                movement_observed = True
            elif classification == "packet_boundary":
                if movement_observed and not packet_complete:
                    packet_complete = True
            elif classification in {"syn_dropped", "unexpected"}:
                if failure is None:
                    failure = (
                        "unexpected event during relative mouse smoke"
                        if classification == "unexpected"
                        else "SYN_DROPPED made mouse evidence incomplete"
                    )
                classification = "syn_dropped" if classification == "syn_dropped" else "unexpected"
            evidence.record(
                phase="mouse_wait",
                batch=current_batch,
                index=index,
                event=event,
                classification=classification,
            )
    elapsed_ms = max(0.0, (clock() - started) * 1000.0)
    if observation is not None:
        observation["movement_observed"] = movement_observed
        observation["packet_complete"] = packet_complete
        observation["latency_ms"] = elapsed_ms if movement_observed else None
    if failure is not None:
        raise MouseSmokeError(failure, exit_code=5)
    if not movement_observed:
        raise MouseSmokeError("timed out waiting for REL_X +1", exit_code=5)
    if not packet_complete:
        raise MouseSmokeError(
            "REL_X +1 observed without SYN_REPORT packet boundary", exit_code=5
        )
    return elapsed_ms, polls


def _mouse_quiet_tail(
    observer: Any,
    timeout: float,
    *,
    evidence: _MouseEventEvidence,
    batch_number: int,
) -> None:
    """Reject any additional mouse motion or input after the target packet."""

    if timeout <= 0:
        return
    failure: str | None = None
    for index, event in enumerate(observer.wait_events(timeout)):
        if event.event_type == EV_SYN and event.code == SYN_REPORT:
            classification = "ignored_syn"
        elif event.event_type == EV_MSC and event.code == MSC_SCAN:
            classification = "metadata"
        elif is_syn_dropped(event):
            classification = "syn_dropped"
            if failure is None:
                failure = "SYN_DROPPED made mouse quiet-tail evidence incomplete"
        else:
            classification = "unexpected"
            if failure is None:
                failure = "unexpected event after relative mouse report"
        evidence.record(
            phase="mouse_tail",
            batch=batch_number,
            index=index,
            event=event,
            classification=classification,
        )
    if failure is not None:
        raise MouseSmokeError(failure, exit_code=5)


def run_mouse_smoke(
    candidate: InputCandidate,
    *,
    serial_port: str,
    baudrate: int = DEFAULT_BAUD,
    event_timeout: float = DEFAULT_EVENT_TIMEOUT,
    quiet_tail: float = DEFAULT_QUIET_TAIL,
    observer_factory: Callable[[Path], Any] = ReadOnlyEventObserver,
    transport_factory: Callable[[str, int, float], Any],
    client_factory: Callable[[Any, float], Any],
    clock: Callable[[], float] = time.monotonic,
) -> tuple[int, dict[str, object]]:
    """Run one bounded, one-shot relative ``REL_X=+1`` mouse transaction."""

    if not serial_port or baudrate <= 0:
        raise ValueError("serial port and positive baudrate are required")
    if (
        not math.isfinite(event_timeout)
        or event_timeout <= 0
        or not math.isfinite(quiet_tail)
        or quiet_tail < 0
    ):
        raise ValueError("event timeout must be positive and quiet tail non-negative")
    started = clock()
    evidence: dict[str, object] = {
        "status": "fail",
        "hardware_opt_in": True,
        "mode": "mouse",
        "observer_state": "not_open",
        "serial_accessed": False,
        "mouse": candidate.as_dict(),
        "report": {"buttons": 0, "x": 1, "y": 0, "wheel": 0, "pan": 0},
        "submit": None,
        "movement_observed": False,
        "movement_latency_ms": None,
        "packet_complete": False,
        "cleanup_attempted": False,
        "cleanup_result": None,
        "route_cleanup": None,
    }
    event_evidence = _MouseEventEvidence()
    event_evidence.update(evidence)
    observer = observer_factory(candidate.path)
    transport: Any | None = None
    client: Any | None = None
    session_started = False
    route_selected = False
    cleanup_error: Exception | None = None
    primary_error: MouseSmokeError | None = None
    batch_number = 0
    try:
        observer.open()
        evidence["observer_state"] = "open"
        evidence["initial_drain_events"] = observer.drain()
        evidence["observer_state"] = "ready"

        transport = transport_factory(serial_port, baudrate, event_timeout)
        evidence["serial_accessed"] = True
        transport.open()
        client = client_factory(transport, event_timeout)
        client.connect()
        session_started = True
        client.hid_route_set("usb")
        route_selected = True
        client.connect()

        result = client.mouse_report(0, 1, 0, 0, 0)
        report_state = _report_state(result)
        evidence["submit"] = report_state
        if report_state != "submitted":
            raise MouseSmokeError(
                "relative mouse report was not freshly submitted", exit_code=5
            )
        observation: dict[str, object] = {}
        try:
            _, batches_used = _wait_for_mouse_movement(
                observer,
                event_timeout,
                clock=clock,
                evidence=event_evidence,
                observation=observation,
                batch_number=batch_number,
            )
        except MouseSmokeError:
            evidence["movement_observed"] = bool(observation.get("movement_observed"))
            evidence["movement_latency_ms"] = observation.get("latency_ms")
            evidence["packet_complete"] = bool(observation.get("packet_complete"))
            raise
        evidence["movement_observed"] = bool(observation.get("movement_observed"))
        evidence["movement_latency_ms"] = observation.get("latency_ms")
        evidence["packet_complete"] = bool(observation.get("packet_complete"))
        batch_number += batches_used
        _mouse_quiet_tail(
            observer,
            quiet_tail,
            evidence=event_evidence,
            batch_number=batch_number,
        )
        evidence["cleanup_attempted"] = True
        try:
            evidence["cleanup_result"] = _release_result(
                client.release_all(), error_type=MouseSmokeError
            )
            evidence["route_cleanup"] = client.hid_route_set("none").active.value
            route_selected = False
        except Exception as exc:
            cleanup_error = exc
        evidence["status"] = "pass"
    except MouseSmokeError as exc:
        primary_error = exc
    except ObserverError as exc:
        primary_error = MouseSmokeError(str(exc), exit_code=5)
    except (OSError, PermissionError) as exc:
        primary_error = MouseSmokeError(f"mouse smoke I/O failed: {exc}", exit_code=4)
    except Exception as exc:
        primary_error = MouseSmokeError(
            f"mouse smoke control operation failed: {exc}", exit_code=3
        )
    finally:
        if session_started and client is not None and not bool(evidence["cleanup_attempted"]):
            evidence["cleanup_attempted"] = True
            try:
                evidence["cleanup_result"] = _release_result(
                    client.release_all(), error_type=MouseSmokeError
                )
            except Exception as exc:
                cleanup_error = exc
        if route_selected and client is not None:
            try:
                evidence["route_cleanup"] = client.hid_route_set("none").active.value
                route_selected = False
            except Exception as exc:
                if cleanup_error is None:
                    cleanup_error = exc
        if client is not None:
            try:
                client.close()
            except Exception as exc:
                if primary_error is None and cleanup_error is None:
                    cleanup_error = exc
        elif transport is not None:
            try:
                transport.close()
            except Exception as exc:
                if primary_error is None and cleanup_error is None:
                    cleanup_error = exc
        try:
            observer.close()
        except Exception as exc:
            if primary_error is None and cleanup_error is None:
                cleanup_error = exc

    evidence["total_duration_ms"] = max(0.0, (clock() - started) * 1000.0)
    event_evidence.update(evidence)
    if primary_error is not None:
        evidence["error"] = str(primary_error)
        if cleanup_error is not None:
            evidence["cleanup_error"] = str(cleanup_error)
        return primary_error.exit_code, evidence
    if cleanup_error is not None:
        evidence["status"] = "fail"
        evidence["cleanup_error"] = str(cleanup_error)
        return 6, evidence
    return 0, evidence


def _parse_cli_integer(value: str) -> int:
    try:
        return int(value, 0) if not value.isdigit() else int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover s3-hidbot Linux HID sources and run gated smoke tests."
    )
    parser.add_argument(
        "--hardware",
        action="store_true",
        help="explicitly opt in to Linux input device discovery/open",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON evidence")
    parser.add_argument("--vid", type=_parse_cli_integer, default=DEFAULT_VID)
    parser.add_argument("--pid", type=_parse_cli_integer, default=DEFAULT_PID)
    parser.add_argument("--product", default=DEFAULT_PRODUCT)
    parser.add_argument("--keyboard-interface", type=_parse_cli_integer, default=0)
    parser.add_argument("--mouse-interface", type=_parse_cli_integer, default=1)
    parser.add_argument("--keyboard-event")
    parser.add_argument("--mouse-event")
    smoke_group = parser.add_mutually_exclusive_group()
    smoke_group.add_argument(
        "--keyboard",
        action="store_true",
        help="after discovery, run the opt-in F24 keyboard smoke transaction",
    )
    smoke_group.add_argument(
        "--mouse",
        action="store_true",
        help="after discovery, run the opt-in REL_X mouse smoke transaction",
    )
    parser.add_argument("--port", help="serial port; otherwise S3_HIDBOT_SERIAL")
    parser.add_argument("--baud", type=_parse_cli_integer, default=DEFAULT_BAUD)
    parser.add_argument(
        "--event-timeout",
        type=float,
        default=DEFAULT_EVENT_TIMEOUT,
        help="bounded wait in seconds for each target event",
    )
    parser.add_argument(
        "--quiet-tail",
        type=float,
        default=DEFAULT_QUIET_TAIL,
        help="bounded post-event duplicate check in seconds",
    )
    # These roots are primarily useful for deterministic tests and lab layouts.
    parser.add_argument("--dev-input-root", type=Path, default=Path("/dev/input"))
    parser.add_argument("--sysfs-root", type=Path, default=Path("/sys"))
    return parser


def _observe(
    candidates: tuple[InputCandidate, InputCandidate],
    observer_factory: Callable[[Path], ReadOnlyEventObserver],
) -> tuple[int, int]:
    observers = [observer_factory(candidate.path) for candidate in candidates]
    drained: list[int] = []
    try:
        for observer in observers:
            observer.open()
        for observer in observers:
            drained.append(observer.drain())
    finally:
        for observer in observers:
            observer.close()
    return drained[0], drained[1]


def _emit_error(message: str, stream: TextIO) -> None:
    print(f"ERROR: {message}", file=stream)


def _default_transport_factory(port: str, baudrate: int, timeout: float) -> Any:
    """Construct the existing serial transport only after observer readiness."""

    from hidbot.serial_transport import PySerialTransport

    return PySerialTransport(
        port,
        baudrate,
        read_timeout=min(0.05, timeout),
        write_timeout=1.0,
    )


def _default_client_factory(transport: Any, timeout: float) -> Any:
    """Construct the existing host Client; no protocol is duplicated here."""

    from hidbot.client import Client

    # A physical smoke report is a single observable action; do not let the
    # client's normal bounded request retry turn one button test into repeats.
    return Client(transport, timeout=timeout, max_attempts=1)


def _emit_keyboard_evidence(
    evidence: dict[str, object], *, as_json: bool, output: TextIO
) -> None:
    if as_json:
        print(json.dumps(evidence, sort_keys=True), file=output)
        return
    if evidence.get("status") == "pass":
        print("PASS: F24 keyboard smoke completed", file=output)
    else:
        print(f"FAIL: {evidence.get('error', 'keyboard smoke failed')}", file=output)
    for key in (
        "keyboard",
        "f24_usage",
        "observer_state",
        "down_submit",
        "down_observed",
        "down_latency_ms",
        "up_submit",
        "up_observed",
        "up_latency_ms",
        "cleanup_attempted",
        "cleanup_result",
        "allowed_repeat_count",
        "repeat_limit_exceeded",
        "repeat_limit_event",
        "event_evidence_truncated",
        "unexpected_event",
        "event_evidence",
        "total_duration_ms",
    ):
        if key in evidence:
            print(f"{key}: {evidence[key]}", file=output)


def _emit_mouse_evidence(
    evidence: dict[str, object], *, as_json: bool, output: TextIO
) -> None:
    if as_json:
        print(json.dumps(evidence, sort_keys=True), file=output)
        return
    if evidence.get("status") == "pass":
        print("PASS: REL_X mouse smoke completed", file=output)
    else:
        print(f"FAIL: {evidence.get('error', 'mouse smoke failed')}", file=output)
    for key in (
        "mouse",
        "report",
        "observer_state",
        "submit",
        "movement_observed",
        "movement_latency_ms",
        "packet_complete",
        "cleanup_attempted",
        "cleanup_result",
        "event_evidence_truncated",
        "unexpected_event",
        "event_evidence",
        "total_duration_ms",
    ):
        if key in evidence:
            print(f"{key}: {evidence[key]}", file=output)


def main(
    argv: list[str] | None = None,
    *,
    platform_name: str | None = None,
    discovery_fn: Callable[..., tuple[InputCandidate, InputCandidate]] = discover_devices,
    keyboard_discovery_fn: Callable[..., InputCandidate] = discover_keyboard,
    mouse_discovery_fn: Callable[..., InputCandidate] = discover_mouse,
    observer_factory: Callable[[Path], ReadOnlyEventObserver] = ReadOnlyEventObserver,
    transport_factory: Callable[[str, int, float], Any] | None = None,
    client_factory: Callable[[Any, float], Any] | None = None,
    clock: Callable[[], float] = time.monotonic,
    output: TextIO | None = None,
    errors: TextIO | None = None,
) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    out = output or sys.stdout
    err = errors or sys.stderr
    if (platform_name or sys.platform) != "linux":
        _emit_error("U5.4.1 observer supports Linux only", err)
        return 2
    if not args.hardware:
        _emit_error("--hardware is required for physical input discovery", err)
        return 2
    if (args.keyboard or args.mouse) and (
        not math.isfinite(args.event_timeout)
        or args.event_timeout <= 0
        or not math.isfinite(args.quiet_tail)
        or args.quiet_tail < 0
    ):
        _emit_error("event timeout must be positive and quiet tail non-negative", err)
        return 2
    if (args.keyboard or args.mouse) and (args.baud <= 0):
        _emit_error("baud must be positive", err)
        return 2
    if args.vid < 0 or args.pid < 0:
        _emit_error("VID/PID must be non-negative", err)
        return 2
    identity = DeviceIdentity(args.vid, args.pid, args.product)
    if args.keyboard or args.mouse:
        serial_port = args.port or os.environ.get("S3_HIDBOT_SERIAL")
        if not serial_port:
            _emit_error("serial port is required via --port or S3_HIDBOT_SERIAL", err)
            return 2
        try:
            if args.keyboard:
                candidate = keyboard_discovery_fn(
                    dev_input_root=args.dev_input_root,
                    sysfs_root=args.sysfs_root,
                    identity=identity,
                    keyboard_interface=args.keyboard_interface,
                    keyboard_event=args.keyboard_event,
                )
            else:
                candidate = mouse_discovery_fn(
                    dev_input_root=args.dev_input_root,
                    sysfs_root=args.sysfs_root,
                    identity=identity,
                    mouse_interface=args.mouse_interface,
                    mouse_event=args.mouse_event,
                )
        except DiscoveryError as exc:
            _emit_error(str(exc), err)
            return 4
        if args.keyboard:
            smoke_code, evidence = run_keyboard_smoke(
                candidate,
                serial_port=serial_port,
                baudrate=args.baud,
                event_timeout=args.event_timeout,
                quiet_tail=args.quiet_tail,
                observer_factory=observer_factory,
                transport_factory=transport_factory or _default_transport_factory,
                client_factory=client_factory or _default_client_factory,
                clock=clock,
            )
            _emit_keyboard_evidence(evidence, as_json=args.json, output=out)
        else:
            smoke_code, evidence = run_mouse_smoke(
                candidate,
                serial_port=serial_port,
                baudrate=args.baud,
                event_timeout=args.event_timeout,
                quiet_tail=args.quiet_tail,
                observer_factory=observer_factory,
                transport_factory=transport_factory or _default_transport_factory,
                client_factory=client_factory or _default_client_factory,
                clock=clock,
            )
            _emit_mouse_evidence(evidence, as_json=args.json, output=out)
        if smoke_code != 0 and not args.json:
            _emit_error(
                str(evidence.get("error", "keyboard/mouse smoke failed")), err
            )
        return smoke_code
    try:
        candidates = discovery_fn(
            dev_input_root=args.dev_input_root,
            sysfs_root=args.sysfs_root,
            identity=identity,
            keyboard_interface=args.keyboard_interface,
            mouse_interface=args.mouse_interface,
            keyboard_event=args.keyboard_event,
            mouse_event=args.mouse_event,
        )
        keyboard_drained, mouse_drained = _observe(candidates, observer_factory)
    except DiscoveryError as exc:
        _emit_error(str(exc), err)
        return 4
    except (ObserverError, OSError, PermissionError) as exc:
        _emit_error(f"read-only event observer failed: {exc}", err)
        return 4

    evidence = {
        "status": "pass",
        "hardware_opt_in": True,
        "observer_state": "ready",
        "observer_opened": True,
        "hid_reports_sent": 0,
        "serial_accessed": False,
        "keyboard": candidates[0].as_dict(),
        "mouse": candidates[1].as_dict(),
        "initial_drain_events": {
            "keyboard": keyboard_drained,
            "mouse": mouse_drained,
        },
    }
    if args.json:
        print(json.dumps(evidence, sort_keys=True), file=out)
    else:
        print("PASS: read-only HID event observers ready", file=out)
        for role, candidate, drained in (
            ("keyboard", candidates[0], keyboard_drained),
            ("mouse", candidates[1], mouse_drained),
        ):
            print(
                f"{role}: /dev/input/{candidate.path.name} "
                f"interface={candidate.interface_number} "
                f"name={candidate.name!r} drained={drained}",
                file=out,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
