#!/usr/bin/env python3
"""Semantic source/profile consistency tests for release firmware assets."""

from __future__ import annotations

import io
import tarfile
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from release_assets import (
    ReleaseAssetError,
    validate_release_asset_directory,
    validate_release_firmware_archive,
)
from release_contract import ReleaseContract


class ReleaseAssetProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.contract = ReleaseContract(
            version="0.3.0",
            firmware_version="0.3.0",
            host_version="0.3.0",
            build_profile="freenove-fnk0099",
        )
        self.manifest = {
            "firmware": {
                "version": "0.3.0",
                "source_revision": "a" * 40,
                "build_profile": "freenove-fnk0099",
            }
        }

    def _archive(self, root: Path, *, basename: str | None = None, bundle_root: str | None = None) -> Path:
        path = root / (basename or self.contract.firmware_archive)
        top = bundle_root or self.contract.firmware_archive.removesuffix(".tar.gz")
        with tarfile.open(path, "w:gz") as archive:
            info = tarfile.TarInfo(f"{top}/manifest.json")
            payload = b"{}\n"
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
        return path

    def test_matching_source_name_root_and_manifest_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = self._archive(Path(temporary))
            with patch("release_assets.verify_bundle_archive", return_value=self.manifest):
                self.assertEqual(
                    validate_release_firmware_archive(
                        archive, self.contract, source_revision="a" * 40
                    ),
                    self.manifest,
                )

    def test_filename_root_manifest_and_revision_mismatches_fail(self) -> None:
        cases = (
            ("filename", {"basename": "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0085.tar.gz"}, self.manifest, "a" * 40),
            ("root", {"bundle_root": "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0085"}, self.manifest, "a" * 40),
            ("profile", {}, {"firmware": {**self.manifest["firmware"], "build_profile": "freenove-fnk0085"}}, "a" * 40),
            ("revision", {}, self.manifest, "b" * 40),
        )
        for name, archive_args, manifest, revision in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                archive = self._archive(Path(temporary), **archive_args)
                with patch("release_assets.verify_bundle_archive", return_value=manifest):
                    with self.assertRaises(ReleaseAssetError):
                        validate_release_firmware_archive(
                            archive, self.contract, source_revision=revision
                        )

    def test_release_wheel_validation_uses_selected_source_module_contract(self) -> None:
        selected_modules = frozenset({"hidbot/selected-source-module.py"})
        contract = replace(self.contract, host_modules=selected_modules)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name in contract.release_assets:
                (directory / name).write_bytes(b"fixture")
            with (
                patch("release_assets._validate_checksum"),
                patch(
                    "release_assets.validate_release_firmware_archive",
                    return_value=self.manifest,
                ),
                patch("release_assets.validate_wheel") as validate_wheel,
                patch("release_assets._validate_sdist"),
            ):
                validate_release_asset_directory(directory, contract)
            validate_wheel.assert_called_once_with(
                directory / contract.host_wheel,
                contract.version,
                required_modules=selected_modules,
            )


if __name__ == "__main__":
    unittest.main()
