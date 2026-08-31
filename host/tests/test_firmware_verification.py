from __future__ import annotations

from dataclasses import replace
from typing import Any
import unittest

from hidbot.firmware_verification import (
    FIRMWARE_IDENTITY_CAPABILITY,
    ArtifactFirmwareIdentity,
    FirmwareIdentityMismatch,
    FirmwareVerificationClassification,
    IdentityUnavailableReason,
    artifact_identity_from_verified_manifest,
    compare_firmware_identity,
)
from hidbot.protocol import FirmwareIdentity, SystemInfo


ARTIFACT = ArtifactFirmwareIdentity(
    project="s3-hidbot",
    target="esp32s3",
    protocol_version=1,
    version="0.1.0-dev",
    source_revision="a" * 40,
    app_elf_sha256="b" * 64,
    build_profile="freenove-fnk0085",
    idf_version="v5.5.4",
)


def identity(*, source_revision: str | None = "a" * 40) -> FirmwareIdentity:
    return FirmwareIdentity(
        version="0.1.0-dev",
        source_revision=source_revision,
        app_elf_sha256="b" * 64,
        build_profile="freenove-fnk0085",
    )


def system_info(*, firmware: FirmwareIdentity | None = None) -> SystemInfo:
    return SystemInfo(
        project="s3-hidbot",
        target="esp32s3",
        idf_version="v5.5.4",
        protocol_version=1,
        firmware=identity() if firmware is None else firmware,
    )


class FirmwareVerificationTests(unittest.TestCase):
    def test_extracts_runtime_comparable_identity_from_verified_manifest_shape(self) -> None:
        manifest: dict[str, Any] = {
            "project": ARTIFACT.project,
            "firmware": {
                "target": ARTIFACT.target,
                "protocol_version": ARTIFACT.protocol_version,
                "version": ARTIFACT.version,
                "source_revision": ARTIFACT.source_revision,
                "build_profile": ARTIFACT.build_profile,
                "idf_version": ARTIFACT.idf_version,
            },
            "runtime_identity": {"app_elf_sha256": ARTIFACT.app_elf_sha256},
        }
        self.assertEqual(artifact_identity_from_verified_manifest(manifest), ARTIFACT)

    def test_exact_match(self) -> None:
        result = compare_firmware_identity(
            ARTIFACT, (FIRMWARE_IDENTITY_CAPABILITY,), system_info()
        )
        self.assertTrue(result.match)
        self.assertEqual(result.classification, FirmwareVerificationClassification.MATCH)
        self.assertEqual(result.mismatches, ())
        self.assertIsNone(result.unavailable_reason)

    def test_each_field_mismatch_has_one_fixed_code(self) -> None:
        cases = (
            ("project", "other", FirmwareIdentityMismatch.PROJECT_MISMATCH),
            ("target", "esp32c6", FirmwareIdentityMismatch.TARGET_MISMATCH),
            ("protocol_version", 2, FirmwareIdentityMismatch.PROTOCOL_VERSION_MISMATCH),
            ("version", "0.2.0", FirmwareIdentityMismatch.FIRMWARE_VERSION_MISMATCH),
            ("source_revision", "c" * 40, FirmwareIdentityMismatch.SOURCE_REVISION_MISMATCH),
            ("app_elf_sha256", "c" * 64, FirmwareIdentityMismatch.ELF_SHA256_MISMATCH),
            ("build_profile", "other-board", FirmwareIdentityMismatch.BUILD_PROFILE_MISMATCH),
            ("idf_version", "v5.6.0", FirmwareIdentityMismatch.IDF_VERSION_MISMATCH),
        )
        for field, value, expected in cases:
            with self.subTest(field=field):
                info = system_info()
                if field in {"version", "source_revision", "app_elf_sha256", "build_profile"}:
                    info = replace(info, firmware=replace(identity(), **{field: value}))
                else:
                    info = replace(info, **{field: value})
                result = compare_firmware_identity(
                    ARTIFACT, (FIRMWARE_IDENTITY_CAPABILITY,), info
                )
                self.assertFalse(result.match)
                self.assertEqual(result.classification, FirmwareVerificationClassification.MISMATCH)
                self.assertEqual(result.mismatches, (expected,))
                self.assertIsNone(result.unavailable_reason)

    def test_multiple_mismatches_are_collected_in_fixed_order(self) -> None:
        info = SystemInfo(
            project="other",
            target="esp32c6",
            idf_version="v5.6.0",
            protocol_version=2,
            firmware=FirmwareIdentity(
                version="0.2.0",
                source_revision="c" * 40,
                app_elf_sha256="d" * 64,
                build_profile="other-board",
            ),
        )
        result = compare_firmware_identity(ARTIFACT, (FIRMWARE_IDENTITY_CAPABILITY,), info)
        self.assertFalse(result.match)
        self.assertEqual(result.classification, FirmwareVerificationClassification.MISMATCH)
        self.assertEqual(
            result.mismatches,
            (
                FirmwareIdentityMismatch.PROJECT_MISMATCH,
                FirmwareIdentityMismatch.TARGET_MISMATCH,
                FirmwareIdentityMismatch.PROTOCOL_VERSION_MISMATCH,
                FirmwareIdentityMismatch.FIRMWARE_VERSION_MISMATCH,
                FirmwareIdentityMismatch.SOURCE_REVISION_MISMATCH,
                FirmwareIdentityMismatch.ELF_SHA256_MISMATCH,
                FirmwareIdentityMismatch.BUILD_PROFILE_MISMATCH,
                FirmwareIdentityMismatch.IDF_VERSION_MISMATCH,
            ),
        )

    def test_missing_identity_capability_is_unavailable_but_keeps_base_diagnostics(self) -> None:
        info = SystemInfo(
            project="other",
            target="esp32s3",
            idf_version="v5.5.4",
            protocol_version=1,
            firmware=None,
        )
        result = compare_firmware_identity(ARTIFACT, (), info)
        self.assertFalse(result.match)
        self.assertEqual(result.classification, FirmwareVerificationClassification.IDENTITY_UNAVAILABLE)
        self.assertEqual(
            result.unavailable_reason,
            IdentityUnavailableReason.FIRMWARE_IDENTITY_CAPABILITY_MISSING,
        )
        self.assertEqual(result.mismatches, (FirmwareIdentityMismatch.PROJECT_MISMATCH,))

    def test_null_source_revision_is_unavailable_and_compares_other_available_fields(self) -> None:
        info = system_info(firmware=replace(identity(source_revision=None), version="0.2.0"))
        result = compare_firmware_identity(ARTIFACT, (FIRMWARE_IDENTITY_CAPABILITY,), info)
        self.assertFalse(result.match)
        self.assertEqual(result.classification, FirmwareVerificationClassification.IDENTITY_UNAVAILABLE)
        self.assertEqual(
            result.unavailable_reason,
            IdentityUnavailableReason.SOURCE_REVISION_UNAVAILABLE,
        )
        self.assertEqual(result.mismatches, (FirmwareIdentityMismatch.FIRMWARE_VERSION_MISMATCH,))


if __name__ == "__main__":
    unittest.main()
