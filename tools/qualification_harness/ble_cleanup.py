"""Fail-closed BLE qualification cleanup sequencing and evidence states."""

from __future__ import annotations

import errno
from collections.abc import Callable, Mapping
from dataclasses import asdict, dataclass
from enum import Enum
from typing import Any

from .core import QualificationError


class BleCleanupPhase(str, Enum):
    WORKLOAD_OBSERVATION = "workload_observation"
    PRE_RETIREMENT_CLEANUP = "pre_retirement_cleanup"
    ALL_UP_PROVEN = "all_up_proven"
    PRE_RETIREMENT_QUIET_TAIL_PROVEN = "pre_retirement_quiet_tail_proven"
    ROUTE_RETIREMENT_REQUESTED = "route_retirement_requested"
    OBSERVER_RETIREMENT_ALLOWED = "observer_retirement_allowed"
    BLE_TERMINAL_REQUESTED = "ble_terminal_requested"
    ROUTE_NONE_DISCONNECT_PROVEN = "route_none_disconnect_proven"
    FINAL_CONTROL_STATE_PROVEN = "final_control_state_proven"


class ObserverTerminalStatus(str, Enum):
    OBSERVATION_COMPLETE = "observation_complete"
    EXPECTED_DEVICE_RETIRED = "expected_device_retired"
    UNEXPECTED_DEVICE_LOSS = "unexpected_device_loss"
    OBSERVER_IO_ERROR = "observer_io_error"


@dataclass(frozen=True, slots=True)
class ObserverOutcome:
    status: ObserverTerminalStatus
    exact_device: bool
    error_name: str | None = None
    relevant_events: int = 0
    unexpected_events: int = 0
    held_keys: int = 0
    held_buttons: int = 0


@dataclass(frozen=True, slots=True)
class FinalBleControlState:
    route_none: bool
    sequence_active: bool
    held_state: str
    ble_connected: bool
    ble_terminal: str


def classify_observer_error(
    error: OSError,
    *,
    phase: BleCleanupPhase,
    exact_device: bool,
    retirement_requested: bool,
    relevant_events: int = 0,
    unexpected_events: int = 0,
    held_keys: int = 0,
    held_buttons: int = 0,
) -> ObserverOutcome:
    """Classify one exact observer failure without broadly suppressing OSError."""

    error_name = errno.errorcode.get(error.errno, "UNKNOWN")
    removable = error.errno in {errno.ENODEV, errno.ENOENT}
    retirement_phase = phase is BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED
    if removable and exact_device and retirement_requested and retirement_phase:
        status = ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED
    elif removable:
        status = ObserverTerminalStatus.UNEXPECTED_DEVICE_LOSS
    else:
        status = ObserverTerminalStatus.OBSERVER_IO_ERROR
    return ObserverOutcome(
        status,
        exact_device,
        error_name=error_name,
        relevant_events=relevant_events,
        unexpected_events=unexpected_events,
        held_keys=held_keys,
        held_buttons=held_buttons,
    )


def observation_complete(
    *,
    relevant_events: int = 0,
    unexpected_events: int = 0,
    held_keys: int = 0,
    held_buttons: int = 0,
) -> ObserverOutcome:
    return ObserverOutcome(
        ObserverTerminalStatus.OBSERVATION_COMPLETE,
        True,
        relevant_events=relevant_events,
        unexpected_events=unexpected_events,
        held_keys=held_keys,
        held_buttons=held_buttons,
    )


def _release_result(value: Any) -> dict[str, str]:
    if isinstance(value, Mapping):
        keyboard, mouse = value.get("keyboard"), value.get("mouse")
    else:
        keyboard, mouse = getattr(value, "keyboard", None), getattr(value, "mouse", None)
    accepted = {"already_up", "submitted"}
    if keyboard not in accepted or mouse not in accepted:
        raise QualificationError("BLE cleanup release_all result is not authoritative")
    return {"keyboard": keyboard, "mouse": mouse}


def _require_quiet(outcome: ObserverOutcome) -> None:
    if outcome.status is not ObserverTerminalStatus.OBSERVATION_COMPLETE:
        raise QualificationError("BLE observer disappeared before retirement was allowed")
    if not outcome.exact_device:
        raise QualificationError("BLE quiet tail did not use the exact device")
    if (
        outcome.relevant_events
        or outcome.unexpected_events
        or outcome.held_keys
        or outcome.held_buttons
    ):
        raise QualificationError("BLE pre-retirement quiet tail is not all-up and quiet")


def _require_all_up(outcome: ObserverOutcome) -> None:
    if outcome.status is not ObserverTerminalStatus.OBSERVATION_COMPLETE:
        raise QualificationError("BLE observer disappeared before ALL_UP proof")
    if not outcome.exact_device:
        raise QualificationError("BLE ALL_UP proof did not use the exact device")
    if outcome.unexpected_events or outcome.held_keys or outcome.held_buttons:
        raise QualificationError("BLE host observer did not prove ALL_UP")


def run_ble_cleanup(
    *,
    attempt_session: Any,
    session_identity: Callable[[Any], tuple[str, str]],
    keep_session_alive: Callable[[Any], None],
    record_mutation_intent: Callable[[str], None],
    release_all: Callable[[Any], Any],
    observe_all_up: Callable[[], ObserverOutcome],
    observe_quiet_tail: Callable[[], ObserverOutcome],
    request_retirement: Callable[[Any], Any],
    observe_retirement: Callable[[], ObserverOutcome],
    acquire_retired_session: Callable[[], Any],
    request_ble_terminal: Callable[[Any], Any],
    read_final_state: Callable[[Any], FinalBleControlState],
    required_ble_terminal: str,
) -> dict[str, Any]:
    """Continue the caller's valid workload session through intentional retirement.

    The caller retains session ownership. This function never closes the attempt session, reacquires it,
    or replays a mutation. After accepted route-none, the caller supplies
    separate authority for final lifecycle cleanup and status. Any exception (including SessionLostError after a
    side effect) loses continuity and must enter separately labelled recovery.
    Observer callbacks must be bounded within the lease; ping validates and
    refreshes the same authority at each boundary without taking over.
    """
    if not required_ble_terminal:
        raise ValueError("required BLE terminal state must be explicit")
    identity = session_identity(attempt_session)
    if len(identity) != 2 or any(not isinstance(value, str) or not value for value in identity):
        raise QualificationError("valid attempt session identity is required")
    phases = [BleCleanupPhase.WORKLOAD_OBSERVATION]

    def checkpoint() -> None:
        if session_identity(attempt_session) != identity:
            raise QualificationError("BLE attempt session continuity lost")
        keep_session_alive(attempt_session)
        if session_identity(attempt_session) != identity:
            raise QualificationError("BLE attempt session continuity lost")

    def mutate(operation: str, action: Callable[[Any], Any]) -> Any:
        checkpoint()
        record_mutation_intent(operation)
        checkpoint()
        result = action(attempt_session)
        checkpoint()
        return result

    try:
        phases.append(BleCleanupPhase.PRE_RETIREMENT_CLEANUP)
        release = _release_result(mutate("release_all", release_all))
        all_up = observe_all_up()
        _require_all_up(all_up)
        checkpoint()
        phases.append(BleCleanupPhase.ALL_UP_PROVEN)

        quiet = observe_quiet_tail()
        _require_quiet(quiet)
        checkpoint()
        phases.append(BleCleanupPhase.PRE_RETIREMENT_QUIET_TAIL_PROVEN)

        # Intentional route-none consumes the attempt authority. Validate up
        # to invocation and require a correlated accepted response, but never
        # demand the session remain alive after its intentional retirement.
        checkpoint()
        record_mutation_intent("route_retirement")
        checkpoint()
        request_retirement(attempt_session)
        phases.append(BleCleanupPhase.ROUTE_RETIREMENT_REQUESTED)
        phases.append(BleCleanupPhase.OBSERVER_RETIREMENT_ALLOWED)
        retired = observe_retirement()
        if retired.status not in {
            ObserverTerminalStatus.OBSERVATION_COMPLETE,
            ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED,
        }:
            raise QualificationError("BLE observer retirement evidence is invalid")
        if not retired.exact_device:
            raise QualificationError("BLE observer retirement did not identify the exact device")
        if retired.status is ObserverTerminalStatus.EXPECTED_DEVICE_RETIRED and retired.error_name not in {
            "ENODEV", "ENOENT",
        }:
            raise QualificationError("BLE observer retirement used an unsupported error")
        if retired.unexpected_events or retired.held_keys or retired.held_buttons:
            raise QualificationError("BLE observer retirement has unexpected held input")

        # The proof interval has ended. Fresh authority here cannot rescue a
        # lost release/ALL_UP/quiet-tail interval or replay an ambiguous retire.
        retired_session = acquire_retired_session()
        record_mutation_intent("ble_terminal")
        request_ble_terminal(retired_session)
        phases.append(BleCleanupPhase.BLE_TERMINAL_REQUESTED)
        final = read_final_state(retired_session)
        if not (
            final.route_none
            and not final.sequence_active
            and final.held_state == "ALL_UP"
            and not final.ble_connected
            and final.ble_terminal == required_ble_terminal
        ):
            raise QualificationError("BLE final control state is not safely retired")
    except QualificationError:
        raise
    except Exception as exc:
        raise QualificationError("BLE cleanup continuity lost; recovery required, no replay") from exc

    phases.append(BleCleanupPhase.ROUTE_NONE_DISCONNECT_PROVEN)
    phases.append(BleCleanupPhase.FINAL_CONTROL_STATE_PROVEN)
    return {
        "schema": 2,
        "status": "pass",
        "phases": [phase.value for phase in phases],
        "same_attempt_session": True,
        "release_all": release,
        "all_up": asdict(all_up),
        "quiet_tail": asdict(quiet),
        "observer_retirement": asdict(retired),
        "final_control_state": asdict(final),
    }
