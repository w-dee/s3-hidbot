#!/usr/bin/env python3
"""Reject tracked machine-local and generated repository paths."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import PurePosixPath
from typing import Iterable, Sequence


class RepositoryHygieneError(RuntimeError):
    """Raised when authoritative Git path inspection fails."""


def _git_bytes(arguments: Sequence[str]) -> bytes:
    try:
        result = subprocess.run(
            ["git", *arguments],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise RepositoryHygieneError(f"could not run git: {exc}") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise RepositoryHygieneError(f"git command failed{suffix}") from exc
    return result.stdout


def forbidden_category(path: str) -> str | None:
    """Classify one repository-relative Git path, without filesystem access."""

    candidate = PurePosixPath(path)
    parts = candidate.parts
    if not path or candidate.is_absolute() or ".." in parts:
        return "invalid repository-relative path"
    if path == ".envrc":
        return "machine-local configuration"
    if "__pycache__" in parts or path.endswith(".pyc"):
        return "Python generated cache"
    if path == "firmware/sdkconfig":
        return "ESP-IDF generated/local state"
    if len(parts) >= 2 and parts[:2] in {
        ("firmware", "build"),
        ("firmware", "artifacts"),
        ("firmware", "managed_components"),
        ("host", "build"),
        ("host", "dist"),
    }:
        return "generated/local build output"
    if len(parts) >= 3 and parts[:2] == ("host", "src") and parts[2].endswith(".egg-info"):
        return "host packaging metadata"
    return None


def forbidden_paths(paths: Iterable[str]) -> list[tuple[str, str]]:
    """Return deterministic forbidden-path diagnostics for a path iterable."""

    findings = [
        (path, category)
        for path in paths
        if (category := forbidden_category(path)) is not None
    ]
    return sorted(findings)


def _nul_paths(arguments: Sequence[str]) -> list[str]:
    return [
        os.fsdecode(value)
        for value in _git_bytes(arguments).split(b"\0")
        if value
    ]


def _tracked_paths() -> list[str]:
    return _nul_paths(["ls-files", "-z", "--"])


def _staged_paths() -> list[str]:
    return _nul_paths(
        [
            "diff",
            "--cached",
            "--name-only",
            "--diff-filter=ACMR",
            "--no-renames",
            "-z",
            "--",
        ]
    )


def _report(mode: str, paths: list[str]) -> int:
    findings = forbidden_paths(paths)
    if findings:
        for path, category in findings:
            print(f"repository hygiene: forbidden {mode} path: {path} ({category})")
        return 1
    print(f"repository hygiene: clean ({len(paths)} {mode} paths scanned)")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--staged", action="store_true", help="inspect staged/index paths")
    modes.add_argument("--tracked", action="store_true", help="inspect all tracked paths")
    args = parser.parse_args(argv)

    try:
        if args.staged:
            return _report("staged", _staged_paths())
        return _report("tracked", _tracked_paths())
    except RepositoryHygieneError as exc:
        print(f"repository hygiene: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
