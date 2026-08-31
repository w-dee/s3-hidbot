#!/usr/bin/env python3
"""Focused offline tests for the canonical host wheel artifact contract."""

from __future__ import annotations

import argparse
import hashlib
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path

from host_artifact import (
    CHECKSUM_BASENAME,
    DIST_INFO,
    REQUIRED_MODULES,
    WHEEL_BASENAME,
    HostArtifactError,
    checksum_text,
    validate_artifact_directory,
)


def _write_valid_artifact(directory: Path) -> Path:
    wheel = directory / WHEEL_BASENAME
    with zipfile.ZipFile(wheel, "w") as archive:
        for module in REQUIRED_MODULES:
            archive.writestr(module, "# fixture\n")
        archive.writestr(
            f"{DIST_INFO}/METADATA",
            "Metadata-Version: 2.1\n"
            "Name: s3-hidbot-host\n"
            "Version: 0.1.0\n"
            "Requires-Python: >=3.11\n"
            "Requires-Dist: pyserial<4,>=3.5\n"
            "Provides-Extra: flash\n"
            "Requires-Dist: esptool<5,>=4.12; extra == \"flash\"\n",
        )
        archive.writestr(
            f"{DIST_INFO}/WHEEL",
            "Wheel-Version: 1.0\nGenerator: fixture\nRoot-Is-Purelib: true\nTag: py3-none-any\n",
        )
        archive.writestr(f"{DIST_INFO}/entry_points.txt", "[console_scripts]\nhidbotctl = hidbot.cli:main\n")
        archive.writestr(f"{DIST_INFO}/RECORD", "")
        archive.writestr(f"{DIST_INFO}/licenses/LICENSE", "MIT License\n")
    digest = hashlib.sha256(wheel.read_bytes()).hexdigest()
    (directory / CHECKSUM_BASENAME).write_text(checksum_text(digest, wheel.name), encoding="ascii")
    return wheel


def _replace_member(directory: Path, member: str, content: bytes | str) -> None:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(directory / WHEEL_BASENAME, "a") as archive:
            archive.writestr(member, content)
    wheel = directory / WHEEL_BASENAME
    (directory / CHECKSUM_BASENAME).write_text(
        checksum_text(hashlib.sha256(wheel.read_bytes()).hexdigest(), wheel.name),
        encoding="ascii",
    )


class HostArtifactTests(unittest.TestCase):
    def artifact(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory()
        directory = Path(temporary.name)
        _write_valid_artifact(directory)
        return temporary, directory

    def assert_rejected(self, mutate) -> None:
        temporary, directory = self.artifact()
        self.addCleanup(temporary.cleanup)
        mutate(directory)
        with self.assertRaises(HostArtifactError):
            validate_artifact_directory(directory)

    def test_valid(self) -> None:
        temporary, directory = self.artifact()
        self.addCleanup(temporary.cleanup)
        result = validate_artifact_directory(directory)
        self.assertEqual(result.wheel.name, WHEEL_BASENAME)

    def test_checksum_mismatch(self) -> None:
        self.assert_rejected(lambda directory: (directory / WHEEL_BASENAME).write_bytes(b"changed"))

    def test_malformed_checksum(self) -> None:
        self.assert_rejected(lambda directory: (directory / CHECKSUM_BASENAME).write_text("invalid\n"))

    def test_checksum_wrong_filename(self) -> None:
        def mutate(directory: Path) -> None:
            digest = hashlib.sha256((directory / WHEEL_BASENAME).read_bytes()).hexdigest()
            (directory / CHECKSUM_BASENAME).write_text(f"{digest}  other.whl\n")

        self.assert_rejected(mutate)

    def test_missing_checksum(self) -> None:
        self.assert_rejected(lambda directory: (directory / CHECKSUM_BASENAME).unlink())

    def test_missing_wheel(self) -> None:
        self.assert_rejected(lambda directory: (directory / WHEEL_BASENAME).unlink())

    def test_multiple_wheels(self) -> None:
        self.assert_rejected(lambda directory: (directory / "extra.whl").write_bytes(b"x"))

    def test_unexpected_file(self) -> None:
        self.assert_rejected(lambda directory: (directory / "README.txt").write_text("unexpected"))

    def test_unexpected_directory(self) -> None:
        self.assert_rejected(lambda directory: (directory / "unexpected").mkdir())

    def test_non_zip_wheel(self) -> None:
        self.assert_rejected(lambda directory: (directory / WHEEL_BASENAME).write_bytes(b"not a zip"))

    def test_wrong_metadata(self) -> None:
        def mutate(directory: Path) -> None:
            _replace_member(
                directory,
                f"{DIST_INFO}/METADATA",
                "Name: other\nVersion: 0.1.0\nRequires-Python: >=3.11\n"
                "Requires-Dist: pyserial<4,>=3.5\n",
            )

        self.assert_rejected(mutate)

    def test_wrong_version(self) -> None:
        def mutate(directory: Path) -> None:
            _replace_member(
                directory,
                f"{DIST_INFO}/METADATA",
                "Name: s3-hidbot-host\nVersion: 9.9.9\nRequires-Python: >=3.11\n"
                "Requires-Dist: pyserial<4,>=3.5\n",
            )

        self.assert_rejected(mutate)

    def test_non_pure_tag(self) -> None:
        def mutate(directory: Path) -> None:
            _replace_member(
                directory,
                f"{DIST_INFO}/WHEEL",
                "Root-Is-Purelib: false\nTag: cp312-cp312-linux_x86_64\n",
            )

        self.assert_rejected(mutate)

    def test_developer_specific_path_in_payload(self) -> None:
        def mutate(directory: Path) -> None:
            private_path = b"/" + b"home/" + b"actual-user/project"
            _replace_member(directory, "hidbot/client.py", private_path)

        self.assert_rejected(mutate)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_directory", nargs="?", type=Path)
    args = parser.parse_args()
    if args.artifact_directory is not None:
        validate_artifact_directory(args.artifact_directory)
        print("PASS: canonical host artifact validation")
        return 0
    result = unittest.main(argv=[__file__], exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
