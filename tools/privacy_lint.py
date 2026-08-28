#!/usr/bin/env python3
"""Detect developer-specific absolute home paths in tracked repository content."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


class PrivacyLintError(RuntimeError):
    """Raised when repository content cannot be inspected safely."""


@dataclass(frozen=True)
class Finding:
    path: str
    line_number: int
    matched_prefix: str


_LINUX_ROOT = b"/" + b"home" + b"/"
_MACOS_ROOT = b"/" + b"Users" + b"/"
_WINDOWS_SEPARATOR = b"\\"
_WINDOWS_USERS = b"Users"
_PLACEHOLDER_USERS = frozenset((b"USER", b"<user>"))

_PATTERNS = (
    re.compile(_LINUX_ROOT + rb"[^/\r\n]+" + re.escape(b"/")),
    re.compile(_MACOS_ROOT + rb"[^/\r\n]+" + re.escape(b"/")),
    re.compile(
        rb"[A-Za-z]:"
        + re.escape(_WINDOWS_SEPARATOR)
        + _WINDOWS_USERS
        + re.escape(_WINDOWS_SEPARATOR)
        + rb"[^\\\r\n]+"
        + re.escape(_WINDOWS_SEPARATOR)
    ),
)

_ALLOWED_UNIX_PREFIXES = frozenset(
    (
        _LINUX_ROOT + b"USER/",
        _LINUX_ROOT + b"<user>/",
        _MACOS_ROOT + b"USER/",
        _MACOS_ROOT + b"<user>/",
    )
)


def _git_bytes(arguments: Sequence[str]) -> bytes:
    try:
        result = subprocess.run(
            ["git", *arguments],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise PrivacyLintError(f"could not run git: {exc}") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise PrivacyLintError(f"git command failed{suffix}") from exc
    return result.stdout


def _repo_root() -> Path:
    return Path(os.fsdecode(_git_bytes(["rev-parse", "--show-toplevel"]).rstrip(b"\0\n")))


def _allowed_prefix(match: bytes) -> bool:
    if match in _ALLOWED_UNIX_PREFIXES:
        return True

    parts = match.split(_WINDOWS_SEPARATOR)
    return (
        len(parts) == 4
        and parts[1] == _WINDOWS_USERS
        and parts[2] in _PLACEHOLDER_USERS
        and parts[3] == b""
    )


def scan_bytes(path: str, content: bytes) -> list[Finding]:
    """Return findings in bytes without requiring UTF-8 decoding."""

    findings: list[Finding] = []
    for line_number, line in enumerate(content.split(b"\n"), start=1):
        for pattern in _PATTERNS:
            for match in pattern.finditer(line):
                matched = match.group(0)
                if not _allowed_prefix(matched):
                    findings.append(
                        Finding(
                            path=path,
                            line_number=line_number,
                            matched_prefix=matched.decode("utf-8", errors="backslashreplace"),
                        )
                    )
    return findings


def _index_entries() -> dict[bytes, tuple[bytes, bytes]]:
    """Return stage-zero index entries as path -> (mode, blob id)."""

    entries: dict[bytes, tuple[bytes, bytes]] = {}
    for raw_entry in _git_bytes(["ls-files", "--stage", "-z", "--"]).split(b"\0"):
        if not raw_entry:
            continue
        try:
            header, path = raw_entry.split(b"\t", 1)
            mode, blob, stage = header.split()
        except ValueError as exc:
            raise PrivacyLintError("could not parse the Git index") from exc
        if stage == b"0":
            entries[path] = (mode, blob)
    return entries


def _staged_files() -> list[tuple[str, bytes]]:
    staged_paths = _git_bytes(
        [
            "diff",
            "--cached",
            "--name-only",
            "--diff-filter=ACMR",
            "--no-renames",
            "-z",
            "--",
        ]
    ).split(b"\0")
    index = _index_entries()
    files: list[tuple[str, bytes]] = []
    for path_bytes in staged_paths:
        if not path_bytes:
            continue
        entry = index.get(path_bytes)
        if entry is None:
            raise PrivacyLintError(
                f"staged path is not available as a stage-zero index entry: {os.fsdecode(path_bytes)}"
            )
        mode, blob = entry
        if mode == b"160000":
            continue
        try:
            content = _git_bytes(["cat-file", "blob", blob.decode("ascii")])
        except PrivacyLintError as exc:
            raise PrivacyLintError(f"could not read staged content: {exc}") from exc
        files.append((os.fsdecode(path_bytes), content))
    return files


def _tracked_files() -> Iterable[tuple[str, bytes]]:
    root = _repo_root()
    for path_bytes in _git_bytes(["ls-files", "-z", "--"]).split(b"\0"):
        if not path_bytes:
            continue
        path = os.fsdecode(path_bytes)
        file_path = root / Path(path)
        try:
            content = file_path.read_bytes()
        except OSError as exc:
            raise PrivacyLintError(f"could not read tracked file {path}: {exc}") from exc
        yield path, content


def _report(mode: str, files: Iterable[tuple[str, bytes]]) -> int:
    findings: list[Finding] = []
    scanned = 0
    for path, content in files:
        scanned += 1
        findings.extend(scan_bytes(path, content))

    if findings:
        for finding in findings:
            print(
                f"{finding.path}:{finding.line_number}: "
                f"developer-specific home path {finding.matched_prefix}"
            )
        return 1

    print(f"privacy lint: clean ({scanned} {mode} file{'s' if scanned != 1 else ''} scanned)")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--staged", action="store_true", help="scan staged/index content")
    modes.add_argument("--tracked", action="store_true", help="scan all tracked worktree files")
    args = parser.parse_args(argv)

    try:
        if args.staged:
            return _report("staged", _staged_files())
        return _report("tracked", _tracked_files())
    except PrivacyLintError as exc:
        print(f"privacy lint: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
