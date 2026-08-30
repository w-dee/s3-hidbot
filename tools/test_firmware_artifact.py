#!/usr/bin/env python3
"""Pure unit tests for the U6.3A firmware artifact contract."""

from __future__ import annotations

import io
import json
import tarfile
import tempfile
from pathlib import Path
from typing import Any, Callable

from firmware_artifact import (
    ArtifactError,
    create_deterministic_tar_gz,
    sha256_file,
    verify_bundle_archive,
    verify_bundle_directory,
    write_deterministic_json,
)


def _base_plan() -> dict[str, Any]:
    return {
        "write_flash_args": ["--flash_mode", "dio", "--flash_size", "2MB", "--flash_freq", "80m"],
        "flash_settings": {"flash_mode": "dio", "flash_size": "2MB", "flash_freq": "80m"},
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0x10000": "application.bin",
        },
        "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin", "encrypted": "false"},
        "app": {"offset": "0x10000", "file": "application.bin", "encrypted": "false"},
        "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false"},
        "extra_esptool_args": {"before": "default_reset", "after": "hard_reset", "stub": True, "chip": "esp32s3"},
    }


def _build_synthetic_bundle(root: Path) -> Path:
    bundle = root / "s3-hidbot-firmware-0.1.0-dev-esp32s3-freenove-fnk0085"
    payloads = {
        "application.bin": b"application image\n",
        "application.elf": b"exact linked elf\n",
        "bootloader/bootloader.bin": b"bootloader image\n",
        "partition_table/partition-table.bin": b"partition image\n",
        "provenance/sdkconfig": b"CONFIG_APP_REPRODUCIBLE_BUILD=y\n",
        "provenance/dependencies.lock": b"version: 5.5.4\ntarget: esp32s3\n",
        "LICENSE": b"MIT License\n",
    }
    for relative, payload in payloads.items():
        path = bundle / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    write_deterministic_json(bundle / "flasher_args.json", _base_plan())
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
    manifest: dict[str, Any] = {
        "artifact_manifest_version": 1,
        "project": "s3-hidbot",
        "firmware": {
            "version": "0.1.0-dev",
            "protocol_version": 1,
            "source_revision": "a" * 40,
            "target": "esp32s3",
            "build_profile": "freenove-fnk0085",
            "idf_version": "v5.5.4",
        },
        "runtime_identity": {"app_elf_sha256": files["application.elf"]["sha256"]},
        "build": {
            "reproducible": True,
            "source_date_epoch": 0,
            "container_image": None,
            "tools": {"compiler": "14.2.0", "cmake": "3.30.2", "ninja": "1.12.1", "python": "3.12.3", "esptool": "4.12.0"},
        },
        "provenance": {
            "dependencies_lock_sha256": files["provenance/dependencies.lock"]["sha256"],
            "effective_sdkconfig_sha256": files["provenance/sdkconfig"]["sha256"],
        },
        "flash_plan": "flasher_args.json",
        "files": files,
    }
    write_deterministic_json(bundle / "manifest.json", manifest)
    checksum_paths = sorted(
        ["manifest.json", *[relative for relative in files if (bundle / relative).is_file()]]
    )
    (bundle / "SHA256SUMS").write_text(
        "".join(f"{sha256_file(bundle / relative)}  {relative}\n" for relative in checksum_paths),
        encoding="ascii",
    )
    return bundle


def _rewrite_manifest(bundle: Path, mutate: Callable[[dict[str, Any]], None]) -> None:
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    mutate(manifest)
    write_deterministic_json(manifest_path, manifest)
    files = manifest["files"]
    checksum_paths = sorted(
        ["manifest.json", *[relative for relative in files if (bundle / relative).is_file()]]
    )
    (bundle / "SHA256SUMS").write_text(
        "".join(f"{sha256_file(bundle / relative)}  {relative}\n" for relative in checksum_paths),
        encoding="ascii",
    )


def _expect_reject(bundle_factory: Callable[[Path], Path], mutate: Callable[[Path], None]) -> None:
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-artifact-test-") as temporary:
        bundle = bundle_factory(Path(temporary))
        mutate(bundle)
        try:
            verify_bundle_directory(bundle)
        except ArtifactError:
            return
        raise AssertionError("malformed artifact was accepted")


def _assert_rejects() -> None:
    _expect_reject(_build_synthetic_bundle, lambda bundle: (bundle / "application.bin").write_bytes(b"tampered"))
    _expect_reject(_build_synthetic_bundle, lambda bundle: (bundle / "application.bin").unlink())
    _expect_reject(_build_synthetic_bundle, lambda bundle: (bundle / "unexpected").write_bytes(b"extra"))

    def duplicate_sum(bundle: Path) -> None:
        sums = (bundle / "SHA256SUMS").read_text(encoding="ascii")
        (bundle / "SHA256SUMS").write_text(sums + sums.splitlines()[0] + "\n", encoding="ascii")

    _expect_reject(_build_synthetic_bundle, duplicate_sum)

    def reordered_sums(bundle: Path) -> None:
        lines = (bundle / "SHA256SUMS").read_text(encoding="ascii").splitlines()
        (bundle / "SHA256SUMS").write_text("\n".join(reversed(lines)) + "\n", encoding="ascii")

    _expect_reject(_build_synthetic_bundle, reordered_sums)
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["files"]["application.bin"].update(sha256="0" * 64)),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["files"].update({"/absolute": manifest["files"].pop("application.bin")})),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["files"].update({"../escape": manifest["files"].pop("application.bin")})),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["runtime_identity"].update(app_elf_sha256=manifest["files"]["application.bin"]["sha256"])),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["firmware"].update(source_revision="bad")),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["firmware"].update(build_profile="bad_profile")),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["firmware"].update(target="esp32")),
    )
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["build"].update(reproducible=False)),
    )

    def missing_flash_image(bundle: Path) -> None:
        plan_path = bundle / "flasher_args.json"
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        del plan["flash_files"]["0x10000"]
        write_deterministic_json(plan_path, plan)
        _rewrite_manifest(bundle, lambda manifest: manifest["files"]["flasher_args.json"].update(sha256=sha256_file(plan_path)))

    _expect_reject(_build_synthetic_bundle, missing_flash_image)

    def outside_flash_image(bundle: Path) -> None:
        plan_path = bundle / "flasher_args.json"
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        plan["app"]["file"] = "../outside.bin"
        write_deterministic_json(plan_path, plan)
        _rewrite_manifest(bundle, lambda manifest: manifest["files"]["flasher_args.json"].update(sha256=sha256_file(plan_path)))

    _expect_reject(_build_synthetic_bundle, outside_flash_image)
    _expect_reject(
        _build_synthetic_bundle,
        lambda bundle: _rewrite_manifest(bundle, lambda manifest: manifest["provenance"].update(dependencies_lock_sha256="0" * 64)),
    )

    def non_reproducible_sdkconfig(bundle: Path) -> None:
        path = bundle / "provenance/sdkconfig"
        path.write_bytes(b"# CONFIG_APP_REPRODUCIBLE_BUILD is not set\n")
        _rewrite_manifest(bundle, lambda manifest: manifest["files"]["provenance/sdkconfig"].update(sha256=sha256_file(path)))

    _expect_reject(_build_synthetic_bundle, non_reproducible_sdkconfig)

    def privacy_payload(bundle: Path) -> None:
        path = bundle / "LICENSE"
        developer_path = b"/".join((b"", b"home", b"alice", b"private"))
        path.write_bytes(b"MIT License " + developer_path + b"\n")
        _rewrite_manifest(bundle, lambda manifest: manifest["files"]["LICENSE"].update(sha256=sha256_file(path)))

    _expect_reject(_build_synthetic_bundle, privacy_payload)


def _assert_archive_safety_and_determinism() -> None:
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-artifact-test-") as temporary:
        root = Path(temporary)
        bundle = _build_synthetic_bundle(root)
        archive_one = root / "one.tar.gz"
        archive_two = root / "two.tar.gz"
        create_deterministic_tar_gz(bundle, archive_one, 0)
        create_deterministic_tar_gz(bundle, archive_two, 0)
        assert archive_one.read_bytes() == archive_two.read_bytes()
        verify_bundle_archive(archive_one)

        malicious = root / "malicious.tar.gz"
        with tarfile.open(malicious, mode="w:gz") as archive:
            info = tarfile.TarInfo("bundle/../escape")
            info.size = 1
            archive.addfile(info, io.BytesIO(b"x"))
        try:
            verify_bundle_archive(malicious)
        except ArtifactError:
            pass
        else:
            raise AssertionError("path traversal archive was accepted")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-artifact-test-") as temporary:
        bundle = _build_synthetic_bundle(Path(temporary))
        verify_bundle_directory(bundle)
    _assert_rejects()
    _assert_archive_safety_and_determinism()
    print("PASS: firmware artifact verifier, privacy, path safety, and deterministic archive tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
