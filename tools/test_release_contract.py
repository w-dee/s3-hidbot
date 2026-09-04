#!/usr/bin/env python3
"""Focused no-network tests for release authority, names, and tag resolution."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from release_contract import (
    ReleaseContractError,
    read_release_contract,
    release_tag,
    resolve_annotated_tag_commit,
    validate_release_tag,
    validate_release_version,
)


ROOT = Path(__file__).resolve().parents[1]


class ReleaseContractTests(unittest.TestCase):
    def test_authoritative_versions_and_names(self) -> None:
        contract = read_release_contract(ROOT)
        self.assertEqual(contract.version, "0.2.0")
        self.assertEqual(contract.tag, "v0.2.0")
        self.assertEqual(
            contract.firmware_archive,
            "s3-hidbot-firmware-0.2.0-esp32s3-freenove-fnk0085.tar.gz",
        )
        self.assertEqual(contract.host_wheel, "s3_hidbot_host-0.2.0-py3-none-any.whl")
        self.assertEqual(contract.host_sdist, "s3_hidbot_host-0.2.0.tar.gz")
        self.assertEqual(len(contract.distributable_assets), 6)

    def test_release_versions_reject_development_and_malformed_values(self) -> None:
        for value in ("0.1.0-dev", "0.1.0+build", "v0.1.0", "01.0.0", "1.0", "1.0.0.0"):
            with self.subTest(value=value), self.assertRaises(ReleaseContractError):
                validate_release_version(value)

    def test_authoritative_firmware_and_host_version_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "firmware").mkdir()
            (root / "host").mkdir()
            (root / "firmware" / "version.txt").write_text("0.1.0\n", encoding="utf-8")
            (root / "host" / "pyproject.toml").write_text(
                "[project]\nname = 's3-hidbot-host'\nversion = '0.1.1'\n",
                encoding="utf-8",
            )
            with self.assertRaises(ReleaseContractError):
                read_release_contract(root)

    def test_tag_must_match_version_exactly(self) -> None:
        self.assertEqual(release_tag("0.1.0"), "v0.1.0")
        self.assertEqual(validate_release_tag("v0.1.0", "0.1.0"), "v0.1.0")
        for tag in ("0.1.0", "v0.1.1", "v0.1.0-dev", "release-0.1.0"):
            with self.subTest(tag=tag), self.assertRaises(ReleaseContractError):
                validate_release_tag(tag, "0.1.0")

    def test_annotated_tag_resolves_to_peeled_commit_not_tag_object(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            self._git(repository, "init")
            self._git(repository, "config", "user.name", "Release test")
            self._git(repository, "config", "user.email", "release-test@example.invalid")
            (repository / "payload").write_text("release fixture\n", encoding="utf-8")
            self._git(repository, "add", "payload")
            self._git(repository, "commit", "-m", "fixture")
            commit = self._git(repository, "rev-parse", "HEAD")
            self._git(repository, "tag", "-a", "v0.1.0", "-m", "annotated fixture")
            tag_object = self._git(repository, "rev-parse", "refs/tags/v0.1.0")
            self.assertNotEqual(tag_object, commit)
            self.assertEqual(
                resolve_annotated_tag_commit(repository, "v0.1.0", "0.1.0"),
                commit,
            )

    def test_lightweight_tag_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            self._git(repository, "init")
            self._git(repository, "config", "user.name", "Release test")
            self._git(repository, "config", "user.email", "release-test@example.invalid")
            (repository / "payload").write_text("release fixture\n", encoding="utf-8")
            self._git(repository, "add", "payload")
            self._git(repository, "commit", "-m", "fixture")
            self._git(repository, "tag", "v0.1.0")
            with self.assertRaises(ReleaseContractError):
                resolve_annotated_tag_commit(repository, "v0.1.0", "0.1.0")

    @staticmethod
    def _git(repository: Path, *arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()


if __name__ == "__main__":
    result = unittest.main(argv=[__file__], exit=False)
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
