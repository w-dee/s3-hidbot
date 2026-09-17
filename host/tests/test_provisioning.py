from __future__ import annotations

import json
import os
import shutil
import tempfile
import unittest
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from unittest import mock

from hidbot.artifact import (
    ArtifactError,
    create_deterministic_tar_gz,
    sha256_file,
    verify_bundle_directory,
    write_deterministic_json,
)
from hidbot.provisioning import (
    FlashPlan,
    InspectedFirmwareBundle,
    ProvisioningPolicyError,
    SupportedProvisioningPlan,
    plan_esptool_v4_args,
    stage_and_inspect_firmware_bundle,
    stage_and_verify_firmware_bundle,
)
from hidbot.legacy_recovery import HistoricalReleaseAuthority, matches_historical_release
import hidbot.provisioning as provisioning


def _plan() -> dict[str, object]:
    return {
        "write_flash_args": [
            "--flash_mode",
            "dio",
            "--flash_size",
            "4MB",
            "--flash_freq",
            "80m",
        ],
        "flash_settings": {"flash_mode": "dio", "flash_size": "4MB", "flash_freq": "80m"},
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0x10000": "application.bin",
        },
        "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin", "encrypted": "false"},
        "app": {"offset": "0x10000", "file": "application.bin", "encrypted": "false"},
        "partition-table": {
            "offset": "0x8000",
            "file": "partition_table/partition-table.bin",
            "encrypted": "false",
        },
        "extra_esptool_args": {
            "before": "default_reset",
            "after": "hard_reset",
            "stub": True,
            "chip": "esp32s3",
        },
    }


def _make_bundle(
    root: Path,
    *,
    profile: str = "freenove-fnk0099",
    version: str = "0.1.0-dev",
    source_revision: str = "a" * 40,
) -> Path:
    bundle = root / f"s3-hidbot-firmware-{version}-esp32s3-{profile}"
    payloads = {
        "application.bin": b"application image\n",
        "application.elf": b"exact linked elf\n",
        "bootloader/bootloader.bin": b"bootloader image\n",
        "partition_table/partition-table.bin": b"partition image\n",
        "provenance/sdkconfig": (
            b"CONFIG_APP_REPRODUCIBLE_BUILD=y\n"
            b"CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\n"
            b"# CONFIG_SPIRAM is not set\n"
        ),
        "provenance/dependencies.lock": b"version: 5.5.4\ntarget: esp32s3\n",
        "LICENSE": b"MIT License\n",
    }
    for relative, payload in payloads.items():
        path = bundle / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    write_deterministic_json(bundle / "flasher_args.json", _plan())
    roles = {
        "application.elf": "application_elf",
        "application.bin": "application_bin",
        "bootloader/bootloader.bin": "bootloader_bin",
        "partition_table/partition-table.bin": "partition_table_bin",
        "flasher_args.json": "flash_plan",
        "provenance/sdkconfig": "effective_sdkconfig",
        "provenance/dependencies.lock": "dependencies_lock",
        "LICENSE": "license",
    }
    files = {
        relative: {"sha256": sha256_file(bundle / relative), "role": role}
        for relative, role in roles.items()
    }
    manifest: dict[str, object] = {
        "artifact_manifest_version": 1,
        "project": "s3-hidbot",
        "firmware": {
            "version": version,
            "protocol_version": 1,
            "source_revision": source_revision,
            "target": "esp32s3",
            "build_profile": profile,
            "idf_version": "v5.5.4",
        },
        "runtime_identity": {"app_elf_sha256": files["application.elf"]["sha256"]},
        "build": {
            "reproducible": True,
            "source_date_epoch": 0,
            "container_image": None,
            "tools": {
                "compiler": "14.2.0",
                "cmake": "3.30.2",
                "ninja": "1.12.1",
                "python": "3.12.3",
                "esptool": "4.12.dev1",
            },
        },
        "provenance": {
            "dependencies_lock_sha256": files["provenance/dependencies.lock"]["sha256"],
            "effective_sdkconfig_sha256": files["provenance/sdkconfig"]["sha256"],
        },
        "flash_plan": "flasher_args.json",
        "files": files,
    }
    write_deterministic_json(bundle / "manifest.json", manifest)
    checksum_paths = sorted(["manifest.json", *roles])
    (bundle / "SHA256SUMS").write_text(
        "".join(f"{sha256_file(bundle / relative)}  {relative}\n" for relative in checksum_paths),
        encoding="ascii",
    )
    return bundle


def _refresh(bundle: Path) -> None:
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plan_path = bundle / manifest["flash_plan"]
    manifest["files"]["flasher_args.json"]["sha256"] = sha256_file(plan_path)
    sdkconfig_path = bundle / "provenance/sdkconfig"
    manifest["files"]["provenance/sdkconfig"]["sha256"] = sha256_file(sdkconfig_path)
    manifest["provenance"]["effective_sdkconfig_sha256"] = manifest["files"]["provenance/sdkconfig"]["sha256"]
    write_deterministic_json(manifest_path, manifest)
    sums = sorted(["manifest.json", *manifest["files"]])
    (bundle / "SHA256SUMS").write_text(
        "".join(f"{sha256_file(bundle / relative)}  {relative}\n" for relative in sums),
        encoding="ascii",
    )


def _set_setting(plan: dict[str, object], name: str, value: str) -> None:
    plan["flash_settings"][name] = value
    flag = f"--{name}"
    arguments = plan["write_flash_args"]
    index = arguments.index(flag)
    arguments[index + 1] = value


def _set_offset(plan: dict[str, object], key: str, value: str) -> None:
    entry = plan[key]
    old = entry["offset"]
    path = entry["file"]
    del plan["flash_files"][old]
    plan["flash_files"][value] = path
    entry["offset"] = value


class _TrackedTemporaryDirectory:
    """Test double that makes staging cleanup directly observable."""

    def __init__(self, path: Path) -> None:
        self._path = path

    def __enter__(self) -> str:
        self._path.mkdir()
        return str(self._path)

    def __exit__(self, exception_type: object, exception: object, traceback: object) -> None:
        shutil.rmtree(self._path)


class ProvisioningTests(unittest.TestCase):
    def test_read_only_inspection_accepts_old_profile_without_flash_authority(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(
                Path(temporary) / "source",
                profile="freenove-fnk0085",
            )
            with stage_and_inspect_firmware_bundle(source) as bundle:
                self.assertIsInstance(bundle, InspectedFirmwareBundle)
                self.assertEqual(bundle.artifact_identity.build_profile, "freenove-fnk0085")
                self.assertFalse(hasattr(bundle, "provisioning_plan"))
                bundle.verify_staged_payloads_unchanged()
            with self.assertRaises(ProvisioningPolicyError):
                with stage_and_verify_firmware_bundle(source):
                    pass

    def test_read_only_inspection_does_not_grant_or_apply_flash_policy(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(Path(temporary) / "source")
            plan_path = source / "flasher_args.json"
            plan = json.loads(plan_path.read_text(encoding="utf-8"))
            plan["app"]["encrypted"] = "true"
            write_deterministic_json(plan_path, plan)
            _refresh(source)
            with stage_and_inspect_firmware_bundle(source) as bundle:
                application = next(
                    image for image in bundle.flash_plan.images if image.role == "application_bin"
                )
                self.assertTrue(application.encrypted)
                self.assertFalse(hasattr(bundle, "provisioning_plan"))
            with self.assertRaises(ProvisioningPolicyError):
                with stage_and_verify_firmware_bundle(source):
                    pass

    def test_directory_is_private_snapshot_and_lifetime_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            source = _make_bundle(root / "source")
            with stage_and_verify_firmware_bundle(source) as bundle:
                self.assertIsInstance(bundle.flash_plan, FlashPlan)
                self.assertIsInstance(bundle.provisioning_plan, SupportedProvisioningPlan)
                self.assertTrue(bundle.staged_root.is_dir())
                self.assertTrue(all(image.path.is_relative_to(bundle.staged_root) for image in bundle.flash_plan.images))
                self.assertNotIn(str(source), {str(image.path) for image in bundle.flash_plan.images})
                (source / "application.bin").write_bytes(b"caller mutation\n")
                shutil.rmtree(source)
                self.assertEqual((bundle.staged_root / "application.bin").read_bytes(), b"application image\n")
                bundle.verify_staged_payloads_unchanged()
                with self.assertRaises(TypeError):
                    bundle.manifest["project"] = "mutated"
            self.assertFalse(bundle.staged_root.exists())

    def test_archive_is_extracted_once_into_private_lifetime(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            source = _make_bundle(root / "source")
            archive = root / "firmware.tar.gz"
            create_deterministic_tar_gz(source, archive, 0)
            with mock.patch.object(
                provisioning,
                "_extract_archive_to",
                wraps=provisioning._extract_archive_to,
            ) as extract:
                with stage_and_verify_firmware_bundle(archive) as bundle:
                    self.assertTrue(bundle.staged_root.is_dir())
                    self.assertTrue(all(image.path.is_relative_to(bundle.staged_root) for image in bundle.flash_plan.images))
                    archive.unlink()
                    self.assertTrue(bundle.staged_root.is_dir())
                self.assertEqual(extract.call_count, 1)

    def test_staged_mutation_is_detected_before_execution(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(Path(temporary) / "source")
            with stage_and_verify_firmware_bundle(source) as bundle:
                (bundle.staged_root / "application.bin").write_bytes(b"changed\n")
                with self.assertRaises(ArtifactError):
                    bundle.verify_staged_payloads_unchanged()

    def test_private_staging_is_cleaned_after_consumer_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(Path(temporary) / "source")
            staged_root: Path | None = None
            with self.assertRaises(RuntimeError):
                with stage_and_verify_firmware_bundle(source) as bundle:
                    staged_root = bundle.staged_root
                    raise RuntimeError("simulated execution failure")
            self.assertIsNotNone(staged_root)
            self.assertFalse(staged_root.exists())

    def test_private_staging_is_cleaned_after_verification_or_policy_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            for name, error_type in (
                ("verification", ArtifactError),
                ("policy", ProvisioningPolicyError),
            ):
                with self.subTest(name=name):
                    source = _make_bundle(root / f"source-{name}")
                    if name == "policy":
                        plan_path = source / "flasher_args.json"
                        plan = json.loads(plan_path.read_text(encoding="utf-8"))
                        _set_setting(plan, "flash_size", "8MB")
                        write_deterministic_json(plan_path, plan)
                        _refresh(source)
                    else:
                        (source / "application.bin").write_bytes(b"changed\n")
                    staging = root / f"staging-{name}"
                    with mock.patch.object(
                        provisioning.tempfile,
                        "TemporaryDirectory",
                        return_value=_TrackedTemporaryDirectory(staging),
                    ):
                        with self.assertRaises(error_type):
                            with stage_and_verify_firmware_bundle(source):
                                pass
                    self.assertFalse(staging.exists())

    def test_directory_snapshot_rejects_links_and_special_files(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            source = _make_bundle(root / "source")
            link = source / "linked"
            link.symlink_to(source / "application.bin")
            with self.assertRaises(ArtifactError):
                with stage_and_verify_firmware_bundle(source):
                    pass
            link.unlink()

            linked_directory = root / "linked-directory"
            linked_directory.mkdir()
            (linked_directory / "payload").write_bytes(b"not a bundle")
            (source / "linked-directory").symlink_to(linked_directory, target_is_directory=True)
            with self.assertRaises(ArtifactError):
                with stage_and_verify_firmware_bundle(source):
                    pass
            (source / "linked-directory").unlink()

            if not hasattr(os, "mkfifo"):
                self.skipTest("mkfifo is unavailable on this platform")
            fifo = source / "fifo"
            os.mkfifo(fifo)
            with self.assertRaises(ArtifactError):
                with stage_and_verify_firmware_bundle(source):
                    pass

    def test_pure_planner_uses_supported_order_and_no_runtime_baud(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(Path(temporary) / "source")
            with stage_and_verify_firmware_bundle(source) as bundle:
                args = plan_esptool_v4_args(bundle.provisioning_plan, "/dev/ch343")
                self.assertEqual(args[:17], (
                    "--chip", "esp32s3", "--port", "/dev/ch343",
                    "--before", "default_reset", "--after", "hard_reset",
                    "write_flash", "--flash_mode", "dio", "--flash_size", "4MB",
                    "--flash_freq", "80m", "0x0", str(bundle.flash_plan.images[0].path),
                ))
                self.assertEqual(args[17], "0x8000")
                self.assertEqual(args[19], "0x10000")
                self.assertNotIn("--baud", args)
                self.assertNotIn("--no-stub", args)

    def test_policy_rejects_unsafe_settings_and_numeric_encryption(self) -> None:
        mutations = (
            ("profile", lambda plan, manifest, sdk: manifest["firmware"].update(build_profile="other")),
            ("size", lambda plan, manifest, sdk: _set_setting(plan, "flash_size", "8MB")),
            ("mode", lambda plan, manifest, sdk: _set_setting(plan, "flash_mode", "qio")),
            ("frequency", lambda plan, manifest, sdk: _set_setting(plan, "flash_freq", "40m")),
            ("before", lambda plan, manifest, sdk: plan["extra_esptool_args"].update(before="no_reset")),
            ("after", lambda plan, manifest, sdk: plan["extra_esptool_args"].update(after="no_reset")),
            ("stub", lambda plan, manifest, sdk: plan["extra_esptool_args"].update(stub=False)),
            ("target", lambda plan, manifest, sdk: plan["extra_esptool_args"].update(chip="esp32")),
            ("boot-offset", lambda plan, manifest, sdk: _set_offset(plan, "bootloader", "0x1000")),
            ("partition-offset", lambda plan, manifest, sdk: _set_offset(plan, "partition-table", "0x9000")),
            ("app-offset", lambda plan, manifest, sdk: _set_offset(plan, "app", "0x20000")),
            ("fourth-image", lambda plan, manifest, sdk: plan["flash_files"].update({"0x20000": "extra.bin"})),
            ("missing-image", lambda plan, manifest, sdk: plan["flash_files"].pop("0x10000")),
            ("encrypted-true", lambda plan, manifest, sdk: plan["app"].update(encrypted=True)),
            ("encrypted-zero", lambda plan, manifest, sdk: plan["app"].update(encrypted=0)),
            ("encrypted-one", lambda plan, manifest, sdk: plan["app"].update(encrypted=1)),
        )
        for name, mutate in mutations:
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
                root = Path(temporary)
                source = _make_bundle(root / "source")
                plan_path = source / "flasher_args.json"
                plan = json.loads(plan_path.read_text(encoding="utf-8"))
                manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
                mutate(plan, manifest, source / "provenance/sdkconfig")
                write_deterministic_json(plan_path, plan)
                write_deterministic_json(source / "manifest.json", manifest)
                _refresh(source)
                if name.startswith("encrypted"):
                    # The generic verifier's current set-membership check
                    # accepts numeric 0/1 through Python bool equality.
                    verify_bundle_directory(source)
                with self.assertRaises((ArtifactError, ProvisioningPolicyError)):
                    with stage_and_verify_firmware_bundle(source):
                        pass

    def test_policy_rejects_memory_profile_without_changing_repository_sdkconfig(self) -> None:
        for payload in (
            b"CONFIG_APP_REPRODUCIBLE_BUILD=y\nCONFIG_ESPTOOLPY_FLASHSIZE_2MB=y\n# CONFIG_SPIRAM is not set\n",
            b"CONFIG_APP_REPRODUCIBLE_BUILD=y\n# CONFIG_SPIRAM is not set\n",
            b"CONFIG_APP_REPRODUCIBLE_BUILD=y\nCONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\nCONFIG_SPIRAM=y\n",
        ):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
                source = _make_bundle(Path(temporary) / "source")
                (source / "provenance/sdkconfig").write_bytes(payload)
                _refresh(source)
                with self.assertRaises(ProvisioningPolicyError):
                    with stage_and_verify_firmware_bundle(source):
                        pass

    def test_historical_release_match_binds_outer_and_inner_authority(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            source = _make_bundle(
                root / "source",
                profile="freenove-fnk0085",
                version="0.3.0",
                source_revision="e" * 40,
            )
            archive = root / "legacy.tar.gz"
            create_deterministic_tar_gz(source, archive, 0)
            manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
            authority = HistoricalReleaseAuthority(
                archive_name=archive.name,
                archive_sha256=sha256_file(archive),
                firmware_version="0.3.0",
                source_revision="e" * 40,
                manifest_sha256=sha256_file(source / "manifest.json"),
                application_elf_sha256=manifest["files"]["application.elf"]["sha256"],
                application_bin_sha256=manifest["files"]["application.bin"]["sha256"],
                build_profile="freenove-fnk0085",
            )
            self.assertTrue(
                matches_historical_release(
                    authority,
                    archive_name=archive.name,
                    archive_snapshot=archive,
                    staged_root=source,
                    manifest=manifest,
                )
            )
            mutations = {
                "filename": {"archive_name": "renamed.tar.gz"},
                "outer": {"archive_sha256": "0" * 64},
                "source": {"source_revision": "f" * 40},
                "manifest": {"manifest_sha256": "1" * 64},
                "elf": {"application_elf_sha256": "2" * 64},
                "bin": {"application_bin_sha256": "3" * 64},
            }
            for name, changes in mutations.items():
                with self.subTest(name=name):
                    altered = replace(authority, **changes)
                    self.assertFalse(
                        matches_historical_release(
                            altered,
                            archive_name=archive.name,
                            archive_snapshot=archive,
                            staged_root=source,
                            manifest=manifest,
                        )
                    )

            altered_archive = root / "altered.tar.gz"
            altered_archive.write_bytes(archive.read_bytes() + b"altered")
            self.assertFalse(
                matches_historical_release(
                    authority,
                    archive_name=archive.name,
                    archive_snapshot=altered_archive,
                    staged_root=source,
                    manifest=manifest,
                )
            )
            repacked = root / "repacked.tar.gz"
            create_deterministic_tar_gz(source, repacked, 1)
            self.assertFalse(
                matches_historical_release(
                    authority,
                    archive_name=archive.name,
                    archive_snapshot=repacked,
                    staged_root=source,
                    manifest=manifest,
                )
            )
            for name, mutate in (
                (
                    "artifact-source",
                    lambda value: value["firmware"].update(source_revision="f" * 40),
                ),
                (
                    "artifact-elf",
                    lambda value: value["runtime_identity"].update(
                        app_elf_sha256="4" * 64
                    ),
                ),
                (
                    "artifact-bin",
                    lambda value: value["files"]["application.bin"].update(
                        sha256="5" * 64
                    ),
                ),
            ):
                with self.subTest(name=name):
                    changed_manifest = deepcopy(manifest)
                    mutate(changed_manifest)
                    self.assertFalse(
                        matches_historical_release(
                            authority,
                            archive_name=archive.name,
                            archive_snapshot=archive,
                            staged_root=source,
                            manifest=changed_manifest,
                        )
                    )

    def test_legacy_recovery_requires_flag_archive_and_exact_recognition(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            root = Path(temporary)
            source = _make_bundle(root / "source", profile="freenove-fnk0085")
            archive = root / "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0085.tar.gz"
            create_deterministic_tar_gz(source, archive, 0)

            with self.assertRaises(ProvisioningPolicyError):
                with stage_and_verify_firmware_bundle(archive):
                    pass
            with mock.patch.object(provisioning, "matches_legacy_v0_3_0", return_value=False):
                with self.assertRaises(ProvisioningPolicyError):
                    with stage_and_verify_firmware_bundle(
                        archive, allow_legacy_v0_3_0_recovery=True
                    ):
                        pass
            with mock.patch.object(provisioning, "matches_legacy_v0_3_0", return_value=True):
                with stage_and_verify_firmware_bundle(
                    archive, allow_legacy_v0_3_0_recovery=True
                ) as bundle:
                    self.assertEqual(bundle.authority, "legacy-v0.3.0")
                    self.assertEqual(bundle.provisioning_plan.build_profile, "freenove-fnk0085")
                with self.assertRaises(ProvisioningPolicyError):
                    with stage_and_verify_firmware_bundle(
                        source, allow_legacy_v0_3_0_recovery=True
                    ):
                        pass

    def test_legacy_flag_does_not_change_current_artifact_semantics(self) -> None:
        with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-test-") as temporary:
            source = _make_bundle(Path(temporary) / "source")
            with mock.patch.object(provisioning, "matches_legacy_v0_3_0") as legacy:
                with stage_and_verify_firmware_bundle(
                    source, allow_legacy_v0_3_0_recovery=True
                ) as bundle:
                    self.assertEqual(bundle.authority, "current")
                    self.assertEqual(bundle.provisioning_plan.build_profile, "freenove-fnk0099")
                legacy.assert_not_called()


if __name__ == "__main__":
    unittest.main()
