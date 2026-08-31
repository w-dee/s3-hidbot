"""Pure artifact-to-runtime firmware identity comparison primitives.

Artifact identity extraction accepts only a manifest returned by the official
artifact verifier.  This module deliberately performs no filesystem, archive,
serial, or protocol parsing work.
"""

from __future__ import annotations

from collections.abc import Collection, Mapping
from dataclasses import dataclass
from enum import Enum
from typing import Any, cast

from .protocol import SystemInfo


FIRMWARE_IDENTITY_CAPABILITY = "firmware.identity-v1"


@dataclass(frozen=True)
class ArtifactFirmwareIdentity:
    """Runtime-comparable fields extracted from a verified artifact manifest."""

    project: str
    target: str
    protocol_version: int
    version: str
    source_revision: str
    app_elf_sha256: str
    build_profile: str
    idf_version: str


class FirmwareVerificationClassification(str, Enum):
    MATCH = "MATCH"
    MISMATCH = "MISMATCH"
    IDENTITY_UNAVAILABLE = "IDENTITY_UNAVAILABLE"


class FirmwareIdentityMismatch(str, Enum):
    PROJECT_MISMATCH = "PROJECT_MISMATCH"
    TARGET_MISMATCH = "TARGET_MISMATCH"
    PROTOCOL_VERSION_MISMATCH = "PROTOCOL_VERSION_MISMATCH"
    FIRMWARE_VERSION_MISMATCH = "FIRMWARE_VERSION_MISMATCH"
    SOURCE_REVISION_MISMATCH = "SOURCE_REVISION_MISMATCH"
    ELF_SHA256_MISMATCH = "ELF_SHA256_MISMATCH"
    BUILD_PROFILE_MISMATCH = "BUILD_PROFILE_MISMATCH"
    IDF_VERSION_MISMATCH = "IDF_VERSION_MISMATCH"


class IdentityUnavailableReason(str, Enum):
    FIRMWARE_IDENTITY_CAPABILITY_MISSING = "FIRMWARE_IDENTITY_CAPABILITY_MISSING"
    SOURCE_REVISION_UNAVAILABLE = "SOURCE_REVISION_UNAVAILABLE"


@dataclass(frozen=True)
class FirmwareVerificationResult:
    """Deterministic comparison outcome for one artifact and validated device info."""

    match: bool
    classification: FirmwareVerificationClassification
    mismatches: tuple[FirmwareIdentityMismatch, ...]
    unavailable_reason: IdentityUnavailableReason | None
    artifact: ArtifactFirmwareIdentity
    device: SystemInfo


def artifact_identity_from_verified_manifest(
    manifest: Mapping[str, Any],
) -> ArtifactFirmwareIdentity:
    """Extract identity from a manifest returned by ``verify_bundle_*``.

    The input is a verified-manifest precondition.  Schema, hash, archive, and
    privacy validation remain exclusively the responsibility of
    :mod:`hidbot.artifact`.
    """

    firmware = cast(Mapping[str, Any], manifest["firmware"])
    runtime_identity = cast(Mapping[str, Any], manifest["runtime_identity"])
    return ArtifactFirmwareIdentity(
        project=cast(str, manifest["project"]),
        target=cast(str, firmware["target"]),
        protocol_version=cast(int, firmware["protocol_version"]),
        version=cast(str, firmware["version"]),
        source_revision=cast(str, firmware["source_revision"]),
        app_elf_sha256=cast(str, runtime_identity["app_elf_sha256"]),
        build_profile=cast(str, firmware["build_profile"]),
        idf_version=cast(str, firmware["idf_version"]),
    )


def compare_firmware_identity(
    artifact_identity: ArtifactFirmwareIdentity,
    hello_capabilities: Collection[str],
    system_info: SystemInfo,
) -> FirmwareVerificationResult:
    """Compare a verified artifact identity with validated ``system.info``.

    A missing identity capability or a null runtime source revision makes
    exact identity unavailable.  Available fields are still compared in the
    documented fixed order to preserve useful deterministic diagnostics.
    """

    if not isinstance(artifact_identity, ArtifactFirmwareIdentity):
        raise TypeError("artifact_identity must be an ArtifactFirmwareIdentity")
    if isinstance(hello_capabilities, (str, bytes, bytearray)) or not isinstance(
        hello_capabilities, Collection
    ):
        raise TypeError("hello_capabilities must be a collection of capability strings")
    if not isinstance(system_info, SystemInfo):
        raise TypeError("system_info must be validated SystemInfo")

    capabilities = frozenset(hello_capabilities)
    mismatches: list[FirmwareIdentityMismatch] = []

    def compare(
        code: FirmwareIdentityMismatch, artifact_value: object, device_value: object
    ) -> None:
        if artifact_value != device_value:
            mismatches.append(code)

    compare(FirmwareIdentityMismatch.PROJECT_MISMATCH, artifact_identity.project, system_info.project)
    compare(FirmwareIdentityMismatch.TARGET_MISMATCH, artifact_identity.target, system_info.target)
    compare(
        FirmwareIdentityMismatch.PROTOCOL_VERSION_MISMATCH,
        artifact_identity.protocol_version,
        system_info.protocol_version,
    )

    unavailable_reason: IdentityUnavailableReason | None = None
    firmware = system_info.firmware
    if FIRMWARE_IDENTITY_CAPABILITY not in capabilities:
        unavailable_reason = IdentityUnavailableReason.FIRMWARE_IDENTITY_CAPABILITY_MISSING
    elif firmware is None:
        raise ValueError("validated identity capability requires a firmware identity")
    else:
        compare(FirmwareIdentityMismatch.FIRMWARE_VERSION_MISMATCH, artifact_identity.version, firmware.version)
        if firmware.source_revision is None:
            unavailable_reason = IdentityUnavailableReason.SOURCE_REVISION_UNAVAILABLE
        else:
            compare(
                FirmwareIdentityMismatch.SOURCE_REVISION_MISMATCH,
                artifact_identity.source_revision,
                firmware.source_revision,
            )
        compare(
            FirmwareIdentityMismatch.ELF_SHA256_MISMATCH,
            artifact_identity.app_elf_sha256,
            firmware.app_elf_sha256,
        )
        compare(
            FirmwareIdentityMismatch.BUILD_PROFILE_MISMATCH,
            artifact_identity.build_profile,
            firmware.build_profile,
        )

    compare(FirmwareIdentityMismatch.IDF_VERSION_MISMATCH, artifact_identity.idf_version, system_info.idf_version)

    if unavailable_reason is not None:
        return FirmwareVerificationResult(
            match=False,
            classification=FirmwareVerificationClassification.IDENTITY_UNAVAILABLE,
            mismatches=tuple(mismatches),
            unavailable_reason=unavailable_reason,
            artifact=artifact_identity,
            device=system_info,
        )
    if mismatches:
        return FirmwareVerificationResult(
            match=False,
            classification=FirmwareVerificationClassification.MISMATCH,
            mismatches=tuple(mismatches),
            unavailable_reason=None,
            artifact=artifact_identity,
            device=system_info,
        )
    return FirmwareVerificationResult(
        match=True,
        classification=FirmwareVerificationClassification.MATCH,
        mismatches=(),
        unavailable_reason=None,
        artifact=artifact_identity,
        device=system_info,
    )
