#!/usr/bin/env python3
"""Single source-of-truth checks for an s3-hidbot public release.

This module is deliberately stdlib-only so Actions jobs, release preparation,
and focused tests all apply the same strict version and tag rules.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path


PROJECT = "s3-hidbot"
TARGET = "esp32s3"
PROFILE = "freenove-fnk0085"
DISTRIBUTION = "s3-hidbot-host"
_RELEASE_VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
_SOURCE_REVISION = re.compile(r"^[0-9a-f]{40}$")


class ReleaseContractError(ValueError):
    """A requested release cannot be identified unambiguously."""


def validate_release_version(value: str) -> str:
    if _RELEASE_VERSION.fullmatch(value) is None:
        raise ReleaseContractError(
            "release version must be strict X.Y.Z without prerelease or build metadata"
        )
    return value


def validate_source_revision(value: str) -> str:
    if _SOURCE_REVISION.fullmatch(value) is None:
        raise ReleaseContractError("source revision must be exactly 40 lowercase hexadecimal characters")
    return value


def release_tag(version: str) -> str:
    return f"v{validate_release_version(version)}"


def validate_release_tag(tag: str, version: str) -> str:
    expected = release_tag(version)
    if tag != expected:
        raise ReleaseContractError(f"release tag must be {expected}, got {tag!r}")
    return tag


@dataclass(frozen=True)
class ReleaseContract:
    """Names and metadata derived only from the two authoritative versions."""

    version: str
    firmware_version: str
    host_version: str

    @property
    def tag(self) -> str:
        return release_tag(self.version)

    @property
    def firmware_archive(self) -> str:
        return f"{PROJECT}-firmware-{self.version}-{TARGET}-{PROFILE}.tar.gz"

    @property
    def host_wheel(self) -> str:
        return f"s3_hidbot_host-{self.version}-py3-none-any.whl"

    @property
    def host_sdist(self) -> str:
        return f"s3_hidbot_host-{self.version}.tar.gz"

    @property
    def distributable_assets(self) -> tuple[str, ...]:
        primary = (self.firmware_archive, self.host_wheel, self.host_sdist)
        return tuple(name for item in primary for name in (item, f"{item}.sha256"))

    @property
    def legal_assets(self) -> tuple[str, ...]:
        primary = ("LICENSE", "THIRD_PARTY_NOTICES.md")
        return tuple(name for item in primary for name in (item, f"{item}.sha256"))

    @property
    def release_assets(self) -> tuple[str, ...]:
        return self.distributable_assets + self.legal_assets


def read_release_contract(source_root: Path) -> ReleaseContract:
    root = source_root.resolve()
    try:
        firmware_version = (root / "firmware" / "version.txt").read_text(encoding="utf-8").strip()
        host_data = tomllib.loads((root / "host" / "pyproject.toml").read_text(encoding="utf-8"))
        host_version = host_data["project"]["version"]
    except (FileNotFoundError, KeyError, TypeError, tomllib.TOMLDecodeError) as exc:
        raise ReleaseContractError("could not read authoritative firmware and host versions") from exc
    if not isinstance(host_version, str):
        raise ReleaseContractError("host project version must be a string")
    firmware_version = validate_release_version(firmware_version)
    host_version = validate_release_version(host_version)
    if firmware_version != host_version:
        raise ReleaseContractError(
            f"combined release requires matching firmware and host versions, got "
            f"{firmware_version} and {host_version}"
        )
    return ReleaseContract(
        version=firmware_version,
        firmware_version=firmware_version,
        host_version=host_version,
    )


def _git(repository: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ReleaseContractError(f"Git command failed: {' '.join(arguments)}") from exc
    return result.stdout.strip()


def resolve_checkout_commit(repository: Path) -> str:
    return validate_source_revision(_git(repository, "rev-parse", "--verify", "HEAD"))


def source_date_epoch(repository: Path, commit: str) -> int:
    revision = validate_source_revision(commit)
    try:
        value = int(_git(repository, "show", "-s", "--format=%ct", revision))
    except ValueError as exc:
        raise ReleaseContractError("commit timestamp is not an integer") from exc
    if value < 0:
        raise ReleaseContractError("commit timestamp must not be negative")
    return value


def resolve_annotated_tag_commit(repository: Path, tag: str, version: str) -> str:
    """Resolve an annotated release tag to its peeled commit, never its tag object."""

    validate_release_tag(tag, version)
    tag_ref = f"refs/tags/{tag}"
    object_type = _git(repository, "cat-file", "-t", tag_ref)
    if object_type != "tag":
        raise ReleaseContractError("release tag must be annotated; lightweight tags are rejected")
    commit = validate_source_revision(_git(repository, "rev-parse", "--verify", f"{tag_ref}^{{commit}}"))
    if _git(repository, "cat-file", "-t", commit) != "commit":
        raise ReleaseContractError("annotated release tag does not resolve to a commit")
    return commit


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--tag", help="validate and resolve an annotated vX.Y.Z tag")
    parser.add_argument("--repository", type=Path, help="Git repository used with --tag")
    parser.add_argument("--expected-commit", help="require a particular checked-out commit")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        contract = read_release_contract(args.source_root)
        result: dict[str, object] = {
            "version": contract.version,
            "tag": contract.tag,
            "firmware_archive": contract.firmware_archive,
            "host_wheel": contract.host_wheel,
            "host_sdist": contract.host_sdist,
            "release_assets": list(contract.release_assets),
        }
        if args.tag is not None:
            if args.repository is None:
                raise ReleaseContractError("--repository is required with --tag")
            commit = resolve_annotated_tag_commit(args.repository, args.tag, contract.version)
            result["source_revision"] = commit
            result["source_date_epoch"] = source_date_epoch(args.repository, commit)
        if args.expected_commit is not None:
            expected = validate_source_revision(args.expected_commit)
            repository = args.repository or args.source_root
            actual = resolve_checkout_commit(repository)
            if actual != expected:
                raise ReleaseContractError(f"checkout commit mismatch: expected {expected}, got {actual}")
            result["checkout_commit"] = actual
    except ReleaseContractError as exc:
        print(f"release contract error: {exc}")
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
