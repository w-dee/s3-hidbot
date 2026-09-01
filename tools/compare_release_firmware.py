#!/usr/bin/env python3
"""Fail closed unless candidate and tag firmware bundles are exact equals."""

from __future__ import annotations

import argparse
import filecmp
from pathlib import Path
from typing import Any

from firmware_artifact import ArtifactError, sha256_file, verify_bundle_archive
from release_contract import ReleaseContractError, validate_source_revision


class FirmwareEqualityError(ValueError):
    """Candidate and tag firmware assets are not the exact same release input."""


def _identity(manifest: dict[str, Any]) -> dict[str, Any]:
    try:
        firmware = manifest["firmware"]
        identity = {
            "version": firmware["version"],
            "source_revision": validate_source_revision(firmware["source_revision"]),
            "target": firmware["target"],
            "build_profile": firmware["build_profile"],
            "idf_version": firmware["idf_version"],
            "protocol_version": firmware["protocol_version"],
            "app_elf_sha256": manifest["runtime_identity"]["app_elf_sha256"],
            "files": manifest["files"],
        }
    except (KeyError, TypeError, ReleaseContractError) as exc:
        raise FirmwareEqualityError("verified bundle does not expose required release identity") from exc
    return identity


def compare_release_firmware(candidate: Path, tagged: Path) -> dict[str, Any]:
    """Verify both inputs before requiring archive and internal identity equality."""

    try:
        candidate_manifest = verify_bundle_archive(candidate)
        tag_manifest = verify_bundle_archive(tagged)
    except ArtifactError as exc:
        raise FirmwareEqualityError("official firmware artifact verification failed") from exc
    candidate_digest = sha256_file(candidate)
    tag_digest = sha256_file(tagged)
    if candidate_digest != tag_digest:
        raise FirmwareEqualityError("archive SHA-256 differs")
    if not filecmp.cmp(candidate, tagged, shallow=False):
        raise FirmwareEqualityError("archive bytes differ despite matching SHA-256")
    candidate_identity = _identity(candidate_manifest)
    tag_identity = _identity(tag_manifest)
    for field in (
        "version",
        "source_revision",
        "target",
        "build_profile",
        "idf_version",
        "protocol_version",
        "app_elf_sha256",
        "files",
    ):
        if candidate_identity[field] != tag_identity[field]:
            raise FirmwareEqualityError(f"verified internal identity differs: {field}")
    return candidate_identity


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("tagged", type=Path)
    args = parser.parse_args()
    try:
        identity = compare_release_firmware(args.candidate, args.tagged)
    except (FirmwareEqualityError, OSError) as exc:
        parser.error(str(exc))
    print(
        "PASS: candidate/tag firmware archives are exact equals "
        f"({identity['version']} {identity['source_revision']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
