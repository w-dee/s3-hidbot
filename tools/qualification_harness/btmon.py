"""Bounded, injectable btmon capture control."""

from __future__ import annotations

import hashlib
import math
import signal
import subprocess
import time
from pathlib import Path
from typing import Any, BinaryIO, Callable

from .core import QualificationError


def _spawn(output: BinaryIO) -> Any:
    return subprocess.Popen(
        ["btmon"],
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def capture_btmon(
    output_path: Path | str,
    *,
    duration: float,
    process_factory: Callable[[BinaryIO], Any] = _spawn,
    sleeper: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    """Capture for one bounded interval and return a path-free compact summary."""

    if not math.isfinite(duration) or duration <= 0 or duration > 3600:
        raise ValueError("btmon duration must be in (0, 3600] seconds")
    path = Path(output_path)
    forced = False
    exit_code: int | None = None
    with path.open("xb") as output:
        process = process_factory(output)
        try:
            sleeper(duration)
            early = process.poll()
            if early is not None:
                exit_code = early
                raise QualificationError("btmon exited before the capture deadline")
            process.send_signal(signal.SIGINT)
            try:
                exit_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                try:
                    exit_code = process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    exit_code = process.wait(timeout=2)
        finally:
            if process.poll() is None:
                forced = True
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
    payload = path.read_bytes()
    text = payload.decode("utf-8", errors="replace")
    return {
        "duration_ms": round(duration * 1000),
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "exit_code": exit_code,
        "forced_stop": forced,
        "service_changed_indications": text.count("Service Changed"),
        "report_map_reads": text.count("Report Map"),
    }
