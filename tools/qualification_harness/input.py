"""Identity-based evdev rediscovery and focused HID checkpoint models."""

from __future__ import annotations

import errno
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from typing import Any

from .core import QualificationError, bounded_poll


EV_SYN = 0
EV_KEY = 1
EV_REL = 2
SYN_REPORT = 0
SYN_DROPPED = 3
REL_X = 0
KEY_F24 = 194


@dataclass(frozen=True, slots=True)
class InputEvent:
    event_type: int
    code: int
    value: int


@dataclass(frozen=True, slots=True)
class EvdevIdentity:
    name: str
    bus: int
    vendor: int
    product: int
    version: int
    role: str


@dataclass(frozen=True, slots=True)
class InputNode:
    event_name: str
    identity: EvdevIdentity

    def __post_init__(self) -> None:
        if not self.event_name.startswith("event") or not self.event_name[5:].isdigit():
            raise ValueError("input evidence records only an event basename")


def rediscover_input(
    discover: Callable[[], Iterable[InputNode]],
    identity: EvdevIdentity,
    *,
    previous: InputNode | None = None,
    timeout: float,
    interval: float,
    clock: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    """Find exactly one fresh node by stable identity, never by stale path."""

    def fetch() -> list[InputNode]:
        matches = [node for node in discover() if node.identity == identity]
        if len(matches) > 1:
            raise QualificationError("evdev identity is ambiguous")
        return matches

    result = bounded_poll(
        fetch,
        lambda matches: len(matches) == 1,
        timeout=timeout,
        interval=interval,
        label="evdev rediscovery",
        clock=clock,
        sleeper=sleeper,
    )
    node = result.value[0]
    return {
        "event_name": node.event_name,
        "identity": {
            "name": identity.name,
            "bus": identity.bus,
            "vendor": identity.vendor,
            "product": identity.product,
            "version": identity.version,
            "role": identity.role,
        },
        "path_changed": previous is not None and previous.event_name != node.event_name,
        "attempts": result.attempts,
        "elapsed_ms": result.elapsed_ms,
    }


def classify_node_access(error: OSError, *, retirement_expected: bool) -> str:
    if error.errno in {errno.ENODEV, errno.ENOENT}:
        if retirement_expected:
            return "expected_retirement"
        raise QualificationError("evdev node disappeared outside a retirement boundary") from error
    raise QualificationError("evdev node access failed") from error


class F24Checkpoint:
    """Accept Linux repeats only inside one exact DOWN-to-UP hold interval."""

    def __init__(self) -> None:
        self._state = "waiting_down"
        self.repeats = 0

    def observe(self, event: InputEvent) -> None:
        if event.event_type != EV_KEY or event.code != KEY_F24:
            return
        if self._state == "waiting_down" and event.value == 1:
            self._state = "held"
        elif self._state == "held" and event.value == 2:
            self.repeats += 1
        elif self._state == "held" and event.value == 0:
            self._state = "released"
        elif self._state == "released" and event.value == 1:
            raise QualificationError("fresh F24 replay observed after release")
        elif event.value == 2:
            raise QualificationError("F24 repeat observed outside held interval")
        else:
            raise QualificationError("F24 event sequence is invalid")

    def finish(self) -> dict[str, Any]:
        if self._state != "released":
            raise QualificationError("F24 DOWN/UP checkpoint is incomplete")
        return {"down": True, "up": True, "repeat_count": self.repeats}


class RelXCheckpoint:
    """Require one REL_X/+1 followed by its SYN_REPORT boundary."""

    def __init__(self) -> None:
        self._state = "waiting_rel"

    def observe(self, event: InputEvent) -> None:
        if event.event_type == EV_SYN and event.code == SYN_DROPPED:
            raise QualificationError("evdev synchronization was dropped")
        if self._state == "waiting_rel":
            if event.event_type == EV_REL and event.code == REL_X:
                if event.value != 1:
                    raise QualificationError("REL_X checkpoint value is not +1")
                self._state = "waiting_syn"
            return
        if self._state == "waiting_syn":
            if event.event_type == EV_SYN and event.code == SYN_REPORT:
                self._state = "complete"
            elif event.event_type != EV_SYN:
                raise QualificationError("unexpected event before REL_X SYN_REPORT")
            return
        if event.event_type == EV_REL and event.code == REL_X:
            raise QualificationError("REL_X replay observed after checkpoint")

    def finish(self) -> dict[str, Any]:
        if self._state != "complete":
            raise QualificationError("REL_X/+1 and SYN_REPORT checkpoint is incomplete")
        return {"rel_x": 1, "syn_report": True}
