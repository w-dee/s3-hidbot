#!/usr/bin/env python3
"""Read-only Linux HID event discovery for the U5.4.1 hardware gate.

This slice deliberately stops after discovering and opening the two evdev
sources.  It never opens the UART, creates a host Client, or sends a HID
report.  Physical access is guarded by the explicit ``--hardware`` flag.
"""

from __future__ import annotations

import argparse
import errno
import json
import os
from dataclasses import dataclass
from pathlib import Path
import re
import selectors
import struct
import sys
from typing import Callable, Iterable, TextIO


DEFAULT_VID = 0x303A
DEFAULT_PID = 0x4008
DEFAULT_PRODUCT = "s3-hidbot"
DEFAULT_KEYBOARD_INTERFACE = 0
DEFAULT_MOUSE_INTERFACE = 1
KEY_F24 = 194
REL_X = 0
EV_KEY = 0x01
EV_REL = 0x02
EV_SYN = 0x00
SYN_REPORT = 0x00
SYN_DROPPED = 0x03
EVENT_NAME = re.compile(r"event[0-9]+\Z")


class DiscoveryError(RuntimeError):
    """The requested HID input topology was not uniquely identified."""


class ObserverError(RuntimeError):
    """An event stream could not be decoded as complete input records."""


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


def _parse_cli_integer(value: str) -> int:
    try:
        return int(value, 0) if not value.isdigit() else int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover s3-hidbot Linux HID event sources (read-only)."
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


def main(
    argv: list[str] | None = None,
    *,
    platform_name: str | None = None,
    discovery_fn: Callable[..., tuple[InputCandidate, InputCandidate]] = discover_devices,
    observer_factory: Callable[[Path], ReadOnlyEventObserver] = ReadOnlyEventObserver,
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
    if args.vid < 0 or args.pid < 0:
        _emit_error("VID/PID must be non-negative", err)
        return 2
    identity = DeviceIdentity(args.vid, args.pid, args.product)
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
