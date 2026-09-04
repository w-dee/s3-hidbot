"""Bounded control, invariant, evidence, and cleanup primitives."""

from __future__ import annotations

import dataclasses
import enum
import json
import math
import re
import time
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
from typing import Any


_BOND_ID = re.compile(r"[0-9a-f]{32}\Z")
_PRIVATE_MARKERS = (
    "/dev/serial/",
    "/dev/tty",
    "/root/",
    "S3_HIDBOT_SERIAL",
    ".envrc",
    "PRIVATE KEY",
)
_LINUX_HOME_ROOT = "/" + "home" + "/"
_MAC_HOME_ROOT = "/" + "Users" + "/"
_PRIVATE_HOME = re.compile(
    rf"(?:{re.escape(_LINUX_HOME_ROOT)}|{re.escape(_MAC_HOME_ROOT)})"
    r"(?!USER/|<user>/)[^/\s]+/"
)
_PRIVATE_HOME_WINDOWS = re.compile(r"[A-Za-z]:\\Users\\(?!USER\\|<user>\\)[^\\\s]+\\")
_BLUETOOTH_ADDRESS = re.compile(r"(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])")
_SECRET_FIELD = re.compile(r"(?:password|passkey|pairing[_-]?secret)", re.IGNORECASE)
_SECRET_ASSIGNMENT = re.compile(r"(?:password|passkey|pairing[_-]?secret)\s*[:=]", re.IGNORECASE)


class QualificationError(RuntimeError):
    """A deterministic harness or qualification invariant failure."""


class PollTimeout(QualificationError):
    """A bounded poll expired with a retained terminal diagnostic."""

    def __init__(self, label: str, attempts: int, elapsed_ms: int, last: Any):
        super().__init__(f"{label} did not converge after {attempts} polls")
        self.label = label
        self.attempts = attempts
        self.elapsed_ms = elapsed_ms
        self.last = last


@dataclass(frozen=True, slots=True)
class PollResult:
    value: Any
    attempts: int
    elapsed_ms: int


def bounded_poll(
    fetch: Callable[[], Any],
    predicate: Callable[[Any], bool],
    *,
    timeout: float,
    interval: float,
    label: str,
    clock: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> PollResult:
    """Poll against one monotonic deadline and retain the final observation."""

    if (
        not math.isfinite(timeout)
        or not math.isfinite(interval)
        or timeout <= 0
        or interval <= 0
        or interval > timeout
        or not label
    ):
        raise ValueError("poll timeout, interval, and label must be bounded")
    started = clock()
    previous = started
    attempts = 0
    last: Any = None
    while True:
        attempts += 1
        last = fetch()
        now = clock()
        if now < previous:
            raise QualificationError(f"{label} clock moved backwards")
        previous = now
        if predicate(last):
            return PollResult(last, attempts, round((now - started) * 1000))
        remaining = timeout - (now - started)
        if remaining <= 0:
            raise PollTimeout(label, attempts, round((now - started) * 1000), last)
        sleeper(min(interval, remaining))


@dataclass(frozen=True, slots=True)
class SessionIdentity:
    sequence: int
    session: str
    boot_id: str


class FreshSessionManager:
    """Own fresh Client instances and invalidate authority at lifecycle edges.

    The factory must configure one bounded transport attempt per Client when
    ``max_attempts`` is greater than one here, so the total remains at most
    three identical attempts.
    """

    def __init__(
        self,
        client_factory: Callable[[float], Any],
        *,
        retryable: tuple[type[BaseException], ...] = (OSError,),
        max_attempts: int = 3,
        attempt_timeout: float = 1.0,
    ) -> None:
        if max_attempts < 1 or max_attempts > 3:
            raise ValueError("serial acquisition allows one to three attempts")
        if not math.isfinite(attempt_timeout) or attempt_timeout <= 0:
            raise ValueError("serial acquisition attempt timeout must be bounded")
        self._factory = client_factory
        self._retryable = retryable
        self._max_attempts = max_attempts
        self._attempt_timeout = attempt_timeout
        self._client: Any = None
        self._identity: SessionIdentity | None = None
        self._sequence = 0

    @property
    def client(self) -> Any:
        if self._client is None or self._identity is None:
            raise QualificationError("fresh UART session required")
        return self._client

    @property
    def identity(self) -> SessionIdentity | None:
        return self._identity

    def invalidate(self) -> None:
        self._identity = None

    def acquire(self) -> SessionIdentity:
        """Repeat only the identical fresh-acquisition procedure, at most 3 times."""

        self.close()
        last_error: BaseException | None = None
        for attempt in range(1, self._max_attempts + 1):
            client = None
            try:
                client = self._factory(self._attempt_timeout)
                hello = client.connect()
                session = getattr(hello, "session", None)
                boot_id = getattr(hello, "boot_id", None)
                if not isinstance(session, str) or not session:
                    raise QualificationError("fresh hello omitted session identity")
                if not isinstance(boot_id, str) or not boot_id:
                    raise QualificationError("fresh hello omitted boot identity")
                self._sequence += 1
                self._client = client
                self._identity = SessionIdentity(self._sequence, session, boot_id)
                return self._identity
            except self._retryable as exc:
                last_error = exc
                if client is not None:
                    try:
                        client.close()
                    except Exception:
                        pass
                if attempt == self._max_attempts:
                    break
            except Exception:
                if client is not None:
                    try:
                        client.close()
                    except Exception:
                        pass
                raise
        raise QualificationError(
            f"UART session acquisition failed after {self._max_attempts} attempts"
        ) from last_error

    def require_current(self, identity: SessionIdentity) -> Any:
        if self._identity != identity:
            raise QualificationError("stale UART session authority")
        return self.client

    def close(self) -> None:
        client, self._client = self._client, None
        self._identity = None
        if client is not None:
            client.close()


def _plain(value: Any) -> Any:
    if dataclasses.is_dataclass(value):
        return {field.name: _plain(getattr(value, field.name)) for field in dataclasses.fields(value)}
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, Mapping):
        return {str(key): _plain(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    return value


def validate_route_checkpoint(value: Any) -> dict[str, Any]:
    snapshot = _plain(value)
    required = {"desired", "active", "generation", "transition", "ready"}
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        raise QualificationError("route checkpoint schema is incoherent")
    desired, active = snapshot["desired"], snapshot["active"]
    if desired not in {"none", "usb", "ble"} or active not in {"none", "usb", "ble"}:
        raise QualificationError("route checkpoint contains an unknown transport")
    if type(snapshot["ready"]) is not bool:
        raise QualificationError("route readiness is invalid")
    if snapshot["transition"] == "stable":
        if desired != active or snapshot["ready"] is not (active != "none"):
            raise QualificationError("stable route checkpoint is incoherent")
    elif snapshot["transition"] == "releasing":
        if active == "none" or snapshot["ready"] is not False:
            raise QualificationError("releasing route checkpoint is incoherent")
    else:
        raise QualificationError("route transition is unknown")
    if type(snapshot["generation"]) is not int or snapshot["generation"] < 0:
        raise QualificationError("route generation is invalid")
    return snapshot


def validate_route_sequence(values: Iterable[Any]) -> list[dict[str, Any]]:
    checkpoints = [validate_route_checkpoint(value) for value in values]
    last_transport: str | None = None
    stable_none_since_transport = True
    for checkpoint in checkpoints:
        if checkpoint["transition"] != "stable":
            continue
        active = checkpoint["active"]
        if active == "none":
            stable_none_since_transport = True
            continue
        if last_transport is not None and active != last_transport and not stable_none_since_transport:
            raise QualificationError("USB/BLE transition omitted stable none")
        last_transport = active
        stable_none_since_transport = False
    return checkpoints


def validate_no_automatic_restore(values: Iterable[Any]) -> list[dict[str, Any]]:
    """Validate a scoped transport-retirement sequence through its quiet tail."""

    checkpoints = validate_route_sequence(values)
    saw_transport = False
    retired = False
    for checkpoint in checkpoints:
        if checkpoint["transition"] != "stable":
            continue
        if checkpoint["active"] == "none" and saw_transport:
            retired = True
        elif checkpoint["active"] != "none":
            if retired:
                raise QualificationError("route restored automatically after retirement")
            saw_transport = True
    if not retired:
        raise QualificationError("route retirement did not reach stable none")
    return checkpoints


def validate_delivery_checkpoint(
    *, expected_route: str, usb_deliveries: int, ble_deliveries: int
) -> dict[str, Any]:
    """Require one logical delivery on the selected route and none on the other."""

    if expected_route not in {"none", "usb", "ble"}:
        raise ValueError("expected route is invalid")
    if any(type(value) is not int or value < 0 for value in (usb_deliveries, ble_deliveries)):
        raise ValueError("delivery counts must be non-negative integers")
    expected = {
        "none": (0, 0),
        "usb": (1, 0),
        "ble": (0, 1),
    }[expected_route]
    if (usb_deliveries, ble_deliveries) != expected:
        raise QualificationError("normal HID delivery was missing, duplicated, or dual-routed")
    return {
        "expected_route": expected_route,
        "usb_deliveries": usb_deliveries,
        "ble_deliveries": ble_deliveries,
        "single_delivery": expected_route != "none",
    }


def validate_ble_exposure(value: Any) -> dict[str, Any]:
    snapshot = _plain(value)
    required = {
        "desired", "observed", "generation", "stack_ready", "advertising",
        "connected", "recovery_required", "last_error",
    }
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        raise QualificationError("BLE exposure schema is incoherent")
    desired, observed = snapshot["desired"], snapshot["observed"]
    if desired not in {"hidden", "exposed"} or observed not in {
        "uninitialized", "enabling", "idle", "advertising", "connected",
        "disabling", "fault",
    }:
        raise QualificationError("BLE lifecycle state is unknown")
    if type(snapshot["generation"]) is not int or snapshot["generation"] < 0:
        raise QualificationError("BLE exposure generation is invalid")
    if any(
        type(snapshot[field]) is not bool
        for field in ("stack_ready", "advertising", "connected", "recovery_required")
    ):
        raise QualificationError("BLE exposure boolean state is invalid")
    if snapshot["advertising"] and snapshot["connected"]:
        raise QualificationError("BLE cannot advertise and be connected")
    if observed in {"enabling", "advertising", "connected"} and desired != "exposed":
        raise QualificationError("BLE exposed lifecycle state has hidden intent")
    if observed in {"uninitialized", "idle", "disabling"} and desired != "hidden":
        raise QualificationError("BLE hidden lifecycle state has exposed intent")
    if observed == "uninitialized" and any(
        snapshot[field] for field in ("stack_ready", "advertising", "connected")
    ):
        raise QualificationError("BLE uninitialized state is incoherent")
    if observed == "advertising" and not (
        desired == "exposed" and snapshot["stack_ready"] is True
        and snapshot["advertising"] is True and snapshot["connected"] is False
    ):
        raise QualificationError("BLE advertising state is incoherent")
    if observed == "connected" and not (
        desired == "exposed" and snapshot["stack_ready"] is True
        and snapshot["advertising"] is False and snapshot["connected"] is True
    ):
        raise QualificationError("BLE connected state is incoherent")
    if observed in {"idle", "uninitialized"} and desired == "hidden" and (
        snapshot["advertising"] is not False or snapshot["connected"] is not False
    ):
        raise QualificationError("BLE hidden state is incoherent")
    if (observed == "fault") is not snapshot["recovery_required"]:
        raise QualificationError("BLE recovery state is incoherent")
    if observed == "fault" and (
        snapshot["advertising"] or snapshot["connected"] or snapshot["last_error"] is None
    ):
        raise QualificationError("BLE fault state is incoherent")
    if snapshot["last_error"] is not None:
        error = snapshot["last_error"]
        if not isinstance(error, dict) or set(error) != {"operation", "code"}:
            raise QualificationError("BLE last-error schema is invalid")
        if error["operation"] not in {"enable", "disable", "runtime"} or type(error["code"]) is not int:
            raise QualificationError("BLE last-error value is invalid")
    return snapshot


def validate_usb_exposure(value: Any) -> dict[str, Any]:
    snapshot = _plain(value)
    required = {
        "desired", "observed", "generation", "mounted", "suspended",
        "keyboard_ready", "mouse_ready", "safety_pending",
        "host_release_uncertain", "recovery_required", "last_error",
    }
    if not isinstance(snapshot, dict) or set(snapshot) != required:
        raise QualificationError("USB exposure schema is incoherent")
    desired, observed = snapshot["desired"], snapshot["observed"]
    if desired not in {"hidden", "exposed"} or observed not in {
        "driver_not_installed", "disconnected", "attaching", "mounted",
        "suspended", "detaching",
    }:
        raise QualificationError("USB lifecycle state is unknown")
    if type(snapshot["generation"]) is not int or snapshot["generation"] < 0:
        raise QualificationError("USB exposure generation is invalid")
    bool_fields = (
        "mounted", "suspended", "keyboard_ready", "mouse_ready",
        "safety_pending", "host_release_uncertain", "recovery_required",
    )
    if any(type(snapshot[field]) is not bool for field in bool_fields):
        raise QualificationError("USB exposure boolean state is invalid")
    if observed in {"attaching", "disconnected", "mounted", "suspended"} and desired != "exposed":
        raise QualificationError("USB exposed lifecycle state has hidden intent")
    if observed in {"driver_not_installed", "detaching"} and desired != "hidden":
        raise QualificationError("USB hidden lifecycle state has exposed intent")
    if observed in {"driver_not_installed", "disconnected", "attaching"} and any(
        snapshot[field] for field in ("mounted", "suspended", "keyboard_ready", "mouse_ready")
    ):
        raise QualificationError("USB unavailable state is incoherent")
    if observed == "mounted" and not (
        snapshot["mounted"] and not snapshot["suspended"]
    ):
        raise QualificationError("USB mounted state is incoherent")
    if observed == "suspended" and not (
        snapshot["mounted"] and snapshot["suspended"]
        and not snapshot["keyboard_ready"] and not snapshot["mouse_ready"]
    ):
        raise QualificationError("USB suspended state is incoherent")
    if (snapshot["keyboard_ready"] or snapshot["mouse_ready"]) and not (
        observed == "mounted" and snapshot["mounted"] and not snapshot["suspended"]
    ):
        raise QualificationError("USB endpoint readiness is incoherent")
    if snapshot["last_error"] is not None:
        error = snapshot["last_error"]
        if not isinstance(error, dict) or set(error) != {"operation", "code"}:
            raise QualificationError("USB last-error schema is invalid")
        if error["operation"] not in {"install", "uninstall"} or type(error["code"]) is not int:
            raise QualificationError("USB last-error value is invalid")
    return snapshot


@dataclass(frozen=True, slots=True)
class BondSnapshot:
    capacity: int
    ids: tuple[str, ...]
    healthy: bool

    @classmethod
    def from_value(cls, value: Any) -> "BondSnapshot":
        snapshot = _plain(value)
        if not isinstance(snapshot, dict) or not isinstance(snapshot.get("bonds"), list):
            raise QualificationError("bond snapshot schema is invalid")
        ids = tuple(sorted(entry.get("bond_id") for entry in snapshot["bonds"] if isinstance(entry, dict)))
        if any(not isinstance(item, str) or _BOND_ID.fullmatch(item) is None for item in ids):
            raise QualificationError("bond snapshot contains an invalid opaque ID")
        if len(ids) != len(set(ids)) or snapshot.get("count") != len(ids):
            raise QualificationError("bond snapshot count or uniqueness is invalid")
        capacity = snapshot.get("capacity")
        available = snapshot.get("available")
        if (
            type(capacity) is not int
            or type(available) is not int
            or capacity < len(ids)
            or available != capacity - len(ids)
        ):
            raise QualificationError("bond capacity accounting is invalid")
        healthy = snapshot.get("healthy")
        if type(healthy) is not bool:
            raise QualificationError("bond health state is invalid")
        return cls(capacity, ids, healthy)

    def compare(self, other: "BondSnapshot") -> dict[str, Any]:
        before, after = set(self.ids), set(other.ids)
        return {
            "exact": (
                self.ids == other.ids
                and self.capacity == other.capacity
                and self.healthy == other.healthy
            ),
            "added": sorted(after - before),
            "removed": sorted(before - after),
            "preserved": sorted(before & after),
            "unexpected_eviction": bool(before - after),
            "unexpected_resurrection": bool(after - before),
        }


def inspect_bluez_paired(
    run: Callable[[list[str]], str], *, target_name: str = "s3-hidbot"
) -> dict[str, Any]:
    """Perform only a fixed read-only paired-device listing; omit all addresses."""

    output = run(["bluetoothctl", "devices", "Paired"])
    matches = 0
    for line in output.splitlines():
        parts = line.strip().split(maxsplit=2)
        if len(parts) == 3 and parts[0] == "Device" and parts[2] == target_name:
            matches += 1
    return {"command": "devices Paired", "target_name": target_name, "match_count": matches}


@dataclass(frozen=True, slots=True)
class StepResult:
    status: str
    classification: str | None = None


def _validate_step(result: StepResult, allowed: set[str], label: str) -> None:
    if result.status not in allowed:
        raise ValueError(f"{label} result status is invalid")


def evaluate_outcome(main: StepResult, cleanup: StepResult) -> dict[str, Any]:
    _validate_step(main, {"pass", "fail"}, "main")
    _validate_step(cleanup, {"pass", "fail", "not_required"}, "cleanup")
    overall = "pass" if main.status == "pass" and cleanup.status in {"pass", "not_required"} else "fail"
    failures = [
        name for name, result in (("main", main), ("cleanup", cleanup))
        if result.status == "fail"
    ]
    return {
        "overall": overall,
        "main": dataclasses.asdict(main),
        "cleanup": dataclasses.asdict(cleanup),
        "failed_parts": failures,
    }


def safe_quiescent_cleanup(acquire: Callable[[], Any]) -> dict[str, Any]:
    """Attempt independent non-destructive safety steps with fresh sessions."""

    steps: list[dict[str, str]] = []

    def run_step(name: str, action: Callable[[Any], Any]) -> None:
        client = None
        try:
            client = acquire()
            action(client)
            steps.append({"name": name, "status": "pass"})
        except Exception as exc:
            steps.append({"name": name, "status": "fail", "classification": type(exc).__name__})
        finally:
            if client is not None:
                try:
                    client.close()
                except Exception as exc:
                    steps.append({"name": name + "_close", "status": "fail", "classification": type(exc).__name__})

    # Safety-only release is attempted exactly once; ambiguous normal HID is
    # never replayed by this framework.
    run_step("release_all", lambda client: client.release_all())
    run_step("route_none", lambda client: client.hid_route_set("none"))
    run_step("usb_hidden", lambda client: client.usb_detach())
    run_step("ble_hidden", lambda client: client.ble_disable())

    final: dict[str, Any] = {}
    client = None
    try:
        client = acquire()
        final["route"] = validate_route_checkpoint(client.hid_route_status())
        final["usb"] = validate_usb_exposure(client.usb_exposure_status())
        final["ble"] = validate_ble_exposure(client.ble_exposure_status())
        if not (
            final["route"]["active"] == "none"
            and final["route"]["desired"] == "none"
            and final["usb"].get("desired") == "hidden"
            and final["usb"].get("mounted") is False
            and final["ble"]["desired"] == "hidden"
            and final["ble"]["advertising"] is False
            and final["ble"]["connected"] is False
        ):
            raise QualificationError("final cleanup state is not quiescent")
        steps.append({"name": "final_state", "status": "pass"})
    except Exception as exc:
        steps.append({"name": "final_state", "status": "fail", "classification": type(exc).__name__})
    finally:
        if client is not None:
            try:
                client.close()
            except Exception as exc:
                steps.append({
                    "name": "final_state_close",
                    "status": "fail",
                    "classification": type(exc).__name__,
                })
    return {
        "status": "pass" if all(step["status"] == "pass" for step in steps) else "fail",
        "steps": steps,
        "final": final,
    }


def _assert_public(value: Any) -> None:
    if isinstance(value, str):
        if (
            any(marker in value for marker in _PRIVATE_MARKERS)
            or _PRIVATE_HOME.search(value)
            or _PRIVATE_HOME_WINDOWS.search(value)
            or _BLUETOOTH_ADDRESS.search(value)
            or _SECRET_ASSIGNMENT.search(value)
        ):
            raise QualificationError("qualification evidence contains private data")
    elif isinstance(value, Mapping):
        for key, item in value.items():
            if _SECRET_FIELD.search(str(key)):
                raise QualificationError("qualification evidence contains a secret field")
            _assert_public(str(key))
            _assert_public(item)
    elif isinstance(value, (tuple, list)):
        for item in value:
            _assert_public(item)


@dataclass(slots=True)
class EvidenceDocument:
    source: Mapping[str, Any]
    started_at: str
    target: Mapping[str, Any] | None = None
    artifact: Mapping[str, Any] | None = None
    stages: list[Mapping[str, Any]] = field(default_factory=list)
    invariants: list[Mapping[str, Any]] = field(default_factory=list)
    route_checkpoints: list[Mapping[str, Any]] = field(default_factory=list)
    bond_snapshots: list[Mapping[str, Any]] = field(default_factory=list)
    input_devices: list[Mapping[str, Any]] = field(default_factory=list)
    hid_checkpoints: list[Mapping[str, Any]] = field(default_factory=list)
    btmon: Mapping[str, Any] | None = None

    def serialize(
        self,
        *,
        duration_ms: int,
        main: StepResult,
        cleanup: StepResult,
        failure_classification: str | None = None,
    ) -> str:
        value = {
            "schema": "s3-hidbot-qualification-evidence",
            "version": 1,
            "source": dict(self.source),
            "target": None if self.target is None else dict(self.target),
            "artifact": None if self.artifact is None else dict(self.artifact),
            "timing": {"started_at": self.started_at, "duration_ms": duration_ms},
            "stages": list(self.stages),
            "invariants": list(self.invariants),
            "route_checkpoints": list(self.route_checkpoints),
            "bond_snapshots": list(self.bond_snapshots),
            "input_devices": list(self.input_devices),
            "hid_checkpoints": list(self.hid_checkpoints),
            "btmon": self.btmon,
            "result": evaluate_outcome(main, cleanup),
            "failure_classification": failure_classification,
        }
        _assert_public(value)
        return json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True) + "\n"
