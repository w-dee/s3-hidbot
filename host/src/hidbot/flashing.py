"""Safe, bounded esptool execution for verified firmware bundles.

The caller must provide the private, policy-checked bundle produced by
``stage_and_verify_firmware_bundle``.  This module never opens a serial port
itself and never speaks the HIDBOT protocol; esptool is the programming-plane
boundary.
"""

from __future__ import annotations

import importlib.metadata
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping

from .errors import FlashExecutionError
from .provisioning import (
    VerifiedFirmwareBundle,
    plan_esptool_v4_args,
)


FLASH_TIMEOUT_SECONDS = 300.0
MAX_FLASH_ATTEMPTS = 3
MAX_DIAGNOSTIC_TAIL_BYTES = 8192
PROCESS_REAP_GRACE_SECONDS = 5.0
ESPTOOL_EXTRA_HINT = "s3-hidbot-host[flash]"


class FlashDependencyError(ValueError):
    """The optional, supported esptool dependency is unavailable."""


@dataclass(frozen=True, slots=True)
class FlashExecutionResult:
    """Public execution summary without private paths or tool output."""

    attempts: int
    chip: str
    image_count: int


@dataclass(frozen=True, slots=True)
class _ProcessOutcome:
    returncode: int
    timed_out: bool = False
    diagnostic_tail: bytes = b""


ProcessRunner = Callable[..., _ProcessOutcome]


def _supported_esptool_version(value: str) -> bool:
    """Accept stable numeric versions in the inclusive/exclusive B2 range."""

    match = re.fullmatch(r"(\d+)\.(\d+)(?:\.(\d+))?", value)
    if match is None:
        return False
    major, minor, patch = (int(part or 0) for part in match.groups())
    return major == 4 and (minor, patch) >= (12, 0)


def require_esptool(
    version_provider: Callable[[str], str] | None = None,
) -> str:
    """Check the optional distribution without importing it."""

    provider = importlib.metadata.version if version_provider is None else version_provider
    try:
        value = provider("esptool")
    except Exception as exc:
        raise FlashDependencyError(
            f"esptool >=4.12,<5 is required; install {ESPTOOL_EXTRA_HINT}"
        ) from exc
    if not isinstance(value, str):
        raise FlashDependencyError(
            f"installed esptool metadata is invalid; install {ESPTOOL_EXTRA_HINT} (>=4.12,<5)"
        )
    if not _supported_esptool_version(value):
        raise FlashDependencyError(
            f"installed esptool version {value!r} is unsupported; "
            f"install {ESPTOOL_EXTRA_HINT} (>=4.12,<5)"
        )
    return value


def _tail(path: object, limit: int = MAX_DIAGNOSTIC_TAIL_BYTES) -> bytes:
    stream = path
    assert hasattr(stream, "seek") and hasattr(stream, "read")
    stream.seek(0, os.SEEK_END)  # type: ignore[attr-defined]
    size = stream.tell()  # type: ignore[attr-defined]
    stream.seek(max(0, size - limit), os.SEEK_SET)  # type: ignore[attr-defined]
    value = stream.read(limit)  # type: ignore[attr-defined]
    return value if isinstance(value, bytes) else bytes(value)


def _terminate_and_reap(process: subprocess.Popen[bytes]) -> None:
    try:
        process.terminate()
    except ProcessLookupError:
        pass
    try:
        process.communicate(timeout=PROCESS_REAP_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        try:
            process.kill()
        except ProcessLookupError:
            pass
        process.communicate()


def _subprocess_runner(
    argv: tuple[str, ...],
    *,
    cwd: Path,
    env: Mapping[str, str],
    timeout: float,
    capture_output: bool,
    shell: bool = False,
) -> _ProcessOutcome:
    if shell:
        raise ValueError("shell execution is forbidden")
    if capture_output:
        with tempfile.TemporaryFile(mode="w+b", dir=cwd) as output:
            process = subprocess.Popen(
                list(argv),
                cwd=str(cwd),
                env=dict(env),
                shell=shell,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
            timed_out = False
            try:
                process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                _terminate_and_reap(process)
            except KeyboardInterrupt:
                _terminate_and_reap(process)
                raise
            return _ProcessOutcome(
                returncode=process.returncode,
                timed_out=timed_out,
                diagnostic_tail=_tail(output),
            )

    process = subprocess.Popen(
        list(argv),
        cwd=str(cwd),
        env=dict(env),
        shell=shell,
    )
    try:
        process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _terminate_and_reap(process)
        return _ProcessOutcome(returncode=process.returncode, timed_out=True)
    except KeyboardInterrupt:
        _terminate_and_reap(process)
        raise
    return _ProcessOutcome(returncode=process.returncode)


def _isolated_execution() -> tuple[tempfile.TemporaryDirectory[str], Path, dict[str, str]]:
    temporary = tempfile.TemporaryDirectory(prefix="s3-hidbot-flash-")
    try:
        root = Path(temporary.name)
        execution_cwd = root / "cwd"
        execution_cwd.mkdir()
        config = root / "esptool.cfg"
        config.write_text("[esptool]\n", encoding="ascii")
        environment = {
            key: value for key, value in os.environ.items() if not key.startswith("ESPTOOL_")
        }
        environment["ESPTOOL_CFGFILE"] = str(config)
        return temporary, execution_cwd, environment
    except BaseException:
        temporary.cleanup()
        raise


def execute_flash(
    bundle: VerifiedFirmwareBundle,
    port: str,
    *,
    json_mode: bool = False,
    process_runner: ProcessRunner | None = None,
    on_retry: Callable[[str], None] | None = None,
    version_provider: Callable[[str], str] | None = None,
) -> FlashExecutionResult:
    """Execute the fixed B2 plan with bounded, identical attempts."""

    require_esptool(version_provider)
    argv = (sys.executable, "-m", "esptool", *plan_esptool_v4_args(bundle.provisioning_plan, port))
    runner = _subprocess_runner if process_runner is None else process_runner
    temporary, execution_cwd, environment = _isolated_execution()
    try:
        for attempt in range(1, MAX_FLASH_ATTEMPTS + 1):
            bundle.verify_staged_payloads_unchanged()
            try:
                outcome = runner(
                    argv,
                    cwd=execution_cwd,
                    env=dict(environment),
                    timeout=FLASH_TIMEOUT_SECONDS,
                    capture_output=json_mode,
                    shell=False,
                )
            except KeyboardInterrupt:
                raise
            except OSError as exc:
                raise FlashExecutionError(
                    "could not start esptool",
                    attempts=attempt,
                ) from exc
            if outcome.returncode == 0 and not outcome.timed_out:
                return FlashExecutionResult(
                    attempts=attempt,
                    chip=bundle.provisioning_plan.target,
                    image_count=len(bundle.provisioning_plan.flash_plan.images),
                )
            if attempt < MAX_FLASH_ATTEMPTS:
                if on_retry is not None and not json_mode:
                    on_retry(
                        f"flash attempt {attempt}/{MAX_FLASH_ATTEMPTS} failed; "
                        "retrying identical operation"
                    )
                continue
            reason = "timed out" if outcome.timed_out else "failed"
            raise FlashExecutionError(
                f"esptool {reason} after {attempt} attempts",
                attempts=attempt,
                timed_out=outcome.timed_out,
                diagnostic_tail=outcome.diagnostic_tail[-MAX_DIAGNOSTIC_TAIL_BYTES:],
            )
        raise AssertionError("flash attempt loop did not return")
    finally:
        temporary.cleanup()
