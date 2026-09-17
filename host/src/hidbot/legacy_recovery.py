"""Exact historical-release authority for bounded recovery provisioning."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .artifact import sha256_file


@dataclass(frozen=True, slots=True)
class HistoricalReleaseAuthority:
    """Immutable identity of one explicitly reviewed published archive."""

    archive_name: str
    archive_sha256: str
    firmware_version: str
    source_revision: str
    manifest_sha256: str
    application_elf_sha256: str
    application_bin_sha256: str
    build_profile: str


LEGACY_V0_3_0 = HistoricalReleaseAuthority(
    archive_name="s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0085.tar.gz",
    archive_sha256="4e9f244b069b668ab5d7b0202e1c63e281e85337e98ae48b3e6331e84f9512dc",
    firmware_version="0.3.0",
    source_revision="ece6ffad013053603ae1d8660504034d251a6d09",
    manifest_sha256="88c3e0092650bc784c1ff80e5d4001146efe291f0b8d353eaa33f8a82a161907",
    application_elf_sha256="ce8fb6a5c1025d9362038770d47a2fa81a3a648f1c144e81db47dd3b64654907",
    application_bin_sha256="eb3a02bb9f61975e3c34ed5f3ecc2fb4713e3342f911d310519c0def5a9e9f45",
    build_profile="freenove-fnk0085",
)


def _payload_hash(manifest: Mapping[str, Any], role: str) -> str | None:
    for entry in manifest["files"].values():
        if entry["role"] == role:
            return str(entry["sha256"])
    return None


def matches_historical_release(
    authority: HistoricalReleaseAuthority,
    *,
    archive_name: str,
    archive_snapshot: Path,
    staged_root: Path,
    manifest: Mapping[str, Any],
) -> bool:
    """Match exact outer bytes and independently checked inner authority."""

    firmware = manifest["firmware"]
    return (
        archive_name == authority.archive_name
        and sha256_file(archive_snapshot) == authority.archive_sha256
        and sha256_file(staged_root / "manifest.json") == authority.manifest_sha256
        and firmware["version"] == authority.firmware_version
        and firmware["source_revision"] == authority.source_revision
        and firmware["build_profile"] == authority.build_profile
        and manifest["runtime_identity"]["app_elf_sha256"]
        == authority.application_elf_sha256
        and _payload_hash(manifest, "application_elf")
        == authority.application_elf_sha256
        and _payload_hash(manifest, "application_bin")
        == authority.application_bin_sha256
    )


def matches_legacy_v0_3_0(
    *,
    archive_name: str,
    archive_snapshot: Path,
    staged_root: Path,
    manifest: Mapping[str, Any],
) -> bool:
    """Recognize only the exact frozen published v0.3.0 archive."""

    return matches_historical_release(
        LEGACY_V0_3_0,
        archive_name=archive_name,
        archive_snapshot=archive_snapshot,
        staged_root=staged_root,
        manifest=manifest,
    )
