#!/usr/bin/env python3
"""Tests for release-candidate/tag firmware byte-equality enforcement."""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import compare_release_firmware as equality
from firmware_artifact import create_deterministic_tar_gz, sha256_file, write_deterministic_json
from test_firmware_artifact import _build_synthetic_bundle


def _refresh_checksums(bundle: Path) -> None:
    paths = sorted(path.relative_to(bundle).as_posix() for path in bundle.rglob("*") if path.is_file())
    paths.remove("SHA256SUMS")
    bundle.joinpath("SHA256SUMS").write_text(
        "".join(f"{sha256_file(bundle / path)}  {path}\n" for path in paths),
        encoding="ascii",
    )


def _archive(bundle: Path, output: Path) -> Path:
    create_deterministic_tar_gz(bundle, output, 0)
    return output


class FirmwareEqualityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bundle = _build_synthetic_bundle(self.root)
        self.candidate = _archive(self.bundle, self.root / "candidate.tar.gz")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_identical_verified_archives_pass(self) -> None:
        tagged = self.root / "tagged.tar.gz"
        tagged.write_bytes(self.candidate.read_bytes())
        result = equality.compare_release_firmware(self.candidate, tagged)
        self.assertEqual(result["version"], "0.1.0")

    def test_one_byte_change_fails(self) -> None:
        tagged = self.root / "tagged.tar.gz"
        tagged.write_bytes(self.candidate.read_bytes() + b"x")
        with self.assertRaises(equality.FirmwareEqualityError):
            equality.compare_release_firmware(self.candidate, tagged)

    def test_malformed_artifact_fails(self) -> None:
        tagged = self.root / "tagged.tar.gz"
        tagged.write_bytes(b"not a firmware artifact")
        with self.assertRaises(equality.FirmwareEqualityError):
            equality.compare_release_firmware(self.candidate, tagged)

    def test_source_revision_mismatch_fails_even_when_filename_is_similar(self) -> None:
        tagged_bundle = self._copy_bundle("source-mismatch")
        manifest = self._manifest(tagged_bundle)
        manifest["firmware"]["source_revision"] = "b" * 40
        write_deterministic_json(tagged_bundle / "manifest.json", manifest)
        _refresh_checksums(tagged_bundle)
        self._assert_internal_identity_rejected(_archive(tagged_bundle, self.root / "tagged.tar.gz"))

    def test_elf_hash_mismatch_fails(self) -> None:
        tagged_bundle = self._copy_bundle("elf-mismatch")
        elf = tagged_bundle / "application.elf"
        elf.write_bytes(b"different exact linked elf\n")
        manifest = self._manifest(tagged_bundle)
        digest = sha256_file(elf)
        manifest["files"]["application.elf"]["sha256"] = digest
        manifest["runtime_identity"]["app_elf_sha256"] = digest
        write_deterministic_json(tagged_bundle / "manifest.json", manifest)
        _refresh_checksums(tagged_bundle)
        self._assert_internal_identity_rejected(_archive(tagged_bundle, self.root / "tagged.tar.gz"))

    def test_profile_mismatch_fails(self) -> None:
        tagged_bundle = self._copy_bundle("profile-mismatch")
        manifest = self._manifest(tagged_bundle)
        manifest["firmware"]["build_profile"] = "other-profile"
        write_deterministic_json(tagged_bundle / "manifest.json", manifest)
        _refresh_checksums(tagged_bundle)
        self._assert_internal_identity_rejected(_archive(tagged_bundle, self.root / "tagged.tar.gz"))

    def _copy_bundle(self, name: str) -> Path:
        destination = self.root / name
        for source in self.bundle.rglob("*"):
            target = destination / source.relative_to(self.bundle)
            if source.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read_bytes())
        return destination

    @staticmethod
    def _manifest(bundle: Path) -> dict:
        import json

        return copy.deepcopy(json.loads((bundle / "manifest.json").read_text(encoding="utf-8")))

    def _assert_internal_identity_rejected(self, tagged: Path) -> None:
        # SHA-256 collisions are not a release mechanism. This isolates the
        # required manifest-field check after both artifacts have passed their
        # independent official verifier.
        with (
            mock.patch.object(equality, "sha256_file", return_value="a" * 64),
            mock.patch.object(equality.filecmp, "cmp", return_value=True),
            self.assertRaises(equality.FirmwareEqualityError),
        ):
            equality.compare_release_firmware(self.candidate, tagged)


if __name__ == "__main__":
    result = unittest.main(argv=[__file__], exit=False)
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
