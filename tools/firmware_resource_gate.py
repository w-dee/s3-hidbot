#!/usr/bin/env python3
"""Measure and enforce the authoritative firmware artifact resource gates."""

from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO


APPLICATION_LIMIT = 664_592
STATIC_RAM_LIMIT = 39_832

_SIZE_FIELDS = frozenset(
    {
        "dram_data",
        "dram_bss",
        "dram_rodata",
        "dram_other",
        "used_dram",
        "dram_total",
        "used_dram_ratio",
        "dram_remain",
        "iram_vectors",
        "iram_text",
        "iram_other",
        "used_iram",
        "iram_total",
        "used_iram_ratio",
        "iram_remain",
        "diram_data",
        "diram_bss",
        "diram_text",
        "diram_vectors",
        "diram_rodata",
        "diram_other",
        "diram_total",
        "used_diram",
        "used_diram_ratio",
        "diram_remain",
        "flash_code",
        "flash_rodata",
        "flash_other",
        "used_flash_non_ram",
        "total_size",
    }
)
_STATIC_RAM_FIELDS = ("dram_data", "dram_bss", "diram_data", "diram_bss")


class ResourceGateError(ValueError):
    """Authoritative resource usage could not be measured or exceeded policy."""


@dataclass(frozen=True)
class ResourceUsage:
    application: int
    static_ram: int


def parse_static_ram(size_json: str) -> int:
    """Parse ESP-IDF v5.5.4 legacy JSON and return DRAM/DIRAM data+bss."""

    try:
        document = json.loads(size_json)
    except json.JSONDecodeError as exc:
        raise ResourceGateError("ESP-IDF size output is not valid JSON") from exc
    if not isinstance(document, dict) or set(document) != _SIZE_FIELDS:
        raise ResourceGateError("ESP-IDF size output has an unexpected schema")
    for field in _SIZE_FIELDS:
        value = document[field]
        if field.endswith("_ratio"):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
                raise ResourceGateError(f"ESP-IDF size field {field} is invalid")
        elif isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ResourceGateError(f"ESP-IDF size field {field} is invalid")
    return sum(document[field] for field in _STATIC_RAM_FIELDS)


def measure_resources(application_bin: Path, map_file: Path, idf_size: Path) -> ResourceUsage:
    """Measure one completed artifact-profile build without rebuilding it."""

    for path, label in (
        (application_bin, "application binary"),
        (map_file, "link map"),
        (idf_size, "ESP-IDF size tool"),
    ):
        if path.is_symlink() or not path.is_file():
            raise ResourceGateError(f"{label} is missing or not a regular file")
    application = application_bin.stat().st_size
    if application <= 0:
        raise ResourceGateError("application binary size is invalid")
    try:
        result = subprocess.run(
            [sys.executable, str(idf_size), "--format", "json", str(map_file)],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ResourceGateError("ESP-IDF static RAM measurement failed") from exc
    return ResourceUsage(application=application, static_ram=parse_static_ram(result.stdout))


def enforce_resource_gate(usage: ResourceUsage, output: TextIO = sys.stdout) -> None:
    """Report both limits and fail after reporting every measured value."""

    failures: list[str] = []
    for name, measured, limit in (
        ("application", usage.application, APPLICATION_LIMIT),
        ("static_ram", usage.static_ram, STATIC_RAM_LIMIT),
    ):
        if isinstance(measured, bool) or not isinstance(measured, int) or measured < 0:
            raise ResourceGateError(f"{name} measurement is invalid")
        remaining = limit - measured
        status = "PASS" if remaining >= 0 else "FAIL"
        print(
            f"RESOURCE_GATE {name} measured={measured} limit={limit} "
            f"remaining={remaining} {status}",
            file=output,
        )
        if remaining < 0:
            failures.append(name)
    if failures:
        raise ResourceGateError(f"firmware resource gate exceeded: {', '.join(failures)}")
    print("RESOURCE_GATE=PASS", file=output)


def measure_and_enforce(application_bin: Path, map_file: Path, idf_size: Path) -> ResourceUsage:
    """Measure a completed build, report its margins, and fail closed."""

    try:
        usage = measure_resources(application_bin, map_file, idf_size)
    except ResourceGateError:
        print(
            f"RESOURCE_GATE application measured=UNAVAILABLE limit={APPLICATION_LIMIT} "
            "remaining=UNAVAILABLE FAIL",
            file=sys.stderr,
        )
        print(
            f"RESOURCE_GATE static_ram measured=UNAVAILABLE limit={STATIC_RAM_LIMIT} "
            "remaining=UNAVAILABLE FAIL",
            file=sys.stderr,
        )
        print("RESOURCE_GATE=FAIL", file=sys.stderr)
        raise
    try:
        enforce_resource_gate(usage)
    except ResourceGateError:
        print("RESOURCE_GATE=FAIL", file=sys.stderr)
        raise
    return usage
