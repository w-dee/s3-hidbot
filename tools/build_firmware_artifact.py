#!/usr/bin/env python3
"""Build one self-contained, provenance-aware firmware artifact.

The caller supplies source revision and SOURCE_DATE_EPOCH explicitly.  This
program never discovers Git state and never reads an existing project build
directory; each IDF build is created in a fresh temporary directory.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from firmware_artifact import (
    ArtifactError,
    IDF_VERSION,
    PROJECT,
    PROTOCOL_VERSION,
    TARGET,
    create_deterministic_tar_gz,
    check_bundle_privacy,
    load_json_bytes,
    sha256_file,
    validate_profile,
    validate_source_date_epoch,
    validate_source_revision,
    validate_tool_version,
    validate_version,
    verify_bundle_archive,
    verify_bundle_directory,
    write_deterministic_json,
)
from firmware_resource_gate import ResourceGateError, measure_and_enforce


_TOOL_VERSION_CANDIDATE = re.compile(
    r"(?<![0-9A-Za-z])v?"
    r"([0-9]+\.[0-9]+(?:\.[0-9]+){0,3}"
    r"(?:(?:\.(?:dev|post)[0-9]+)|(?:a|b|rc)[0-9]+)?)"
    r"(?![0-9A-Za-z.])"
)


def _extract_version_number(output: str, label: str) -> str:
    match = _TOOL_VERSION_CANDIDATE.search(output)
    if match is None:
        raise ArtifactError(f"could not normalize {label} version")
    return validate_tool_version(match.group(1))


def _version_number(command: list[str], label: str) -> str:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ArtifactError(f"could not obtain {label} version") from exc
    return _extract_version_number(result.stdout + result.stderr, label)


def _copy_regular(source: Path, destination: Path) -> None:
    if source.is_symlink() or not source.is_file():
        raise ArtifactError(f"build output is not a regular file: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def _profile_from_source(source_root: Path) -> str:
    header = source_root / "firmware/components/firmware_identity/include/firmware_identity/firmware_identity.hpp"
    match = re.search(r'kBuildProfile\s*=\s*"([^"]+)"', header.read_text(encoding="utf-8"))
    if match is None:
        raise ArtifactError("firmware build profile constant is missing")
    return validate_profile(match.group(1))


def _protocol_version_from_source(source_root: Path) -> int:
    header = source_root / "firmware/components/control_protocol/include/control_protocol/control_protocol.hpp"
    match = re.search(r"kProtocolVersion\s*=\s*([0-9]+)", header.read_text(encoding="utf-8"))
    if match is None or int(match.group(1)) != PROTOCOL_VERSION:
        raise ArtifactError("unexpected protocol version source contract")
    return PROTOCOL_VERSION


def _privacy_markers(source_root: Path) -> tuple[bytes, ...]:
    values = (
        str(source_root),
        os.getcwd(),
        os.environ.get("HOME", ""),
        os.environ.get("S3_HIDBOT_SERIAL", ""),
    )
    return tuple(value.encode("utf-8") for value in values if value)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a provenance-aware s3-hidbot firmware artifact")
    parser.add_argument("--output", required=True, type=Path, help="output .tar.gz path")
    parser.add_argument("--source-revision", required=True, help="explicit 40-character source revision")
    parser.add_argument("--source-date-epoch", required=True, type=int, help="explicit archive/build timestamp")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--container-image", default=None, help="null locally, or an immutable image@sha256 reference")
    return parser.parse_args()


def build(args: argparse.Namespace) -> Path:
    source_root = args.source_root.resolve()
    firmware_root = source_root / "firmware"
    if not firmware_root.is_dir():
        raise ArtifactError("source root does not contain firmware/")
    source_revision = validate_source_revision(args.source_revision)
    source_date_epoch = validate_source_date_epoch(args.source_date_epoch)
    version = validate_version((firmware_root / "version.txt").read_text(encoding="utf-8").strip())
    profile = _profile_from_source(source_root)
    protocol_version = _protocol_version_from_source(source_root)
    output = args.output.resolve()
    if output.exists():
        raise ArtifactError("refusing to overwrite an existing artifact")

    idf_path = os.environ.get("IDF_PATH", "")
    idf_py = shutil.which("idf.py")
    if not idf_path or idf_py is None:
        raise ArtifactError("ESP-IDF v5.5.4 must be active (IDF_PATH and idf.py are required)")
    idf_version = _version_number([idf_py, "--version"], "ESP-IDF")
    if idf_version != "5.5.4":
        raise ArtifactError(f"expected ESP-IDF v5.5.4, got {idf_version}")
    compiler = shutil.which("xtensa-esp32s3-elf-gcc") or shutil.which("xtensa-esp-elf-gcc")
    if compiler is None:
        raise ArtifactError("ESP32-S3 compiler is unavailable")

    with tempfile.TemporaryDirectory(prefix="s3-hidbot-artifact-build-") as temporary:
        temporary_root = Path(temporary)
        build_dir = temporary_root / "idf-build"
        sdkconfig = temporary_root / "sdkconfig"
        environment = os.environ.copy()
        environment.update(
            {
                "IDF_TARGET": TARGET,
                "S3_HIDBOT_SOURCE_REVISION": source_revision,
                "SOURCE_DATE_EPOCH": str(source_date_epoch),
                "SDKCONFIG_DEFAULTS": f"{firmware_root / 'sdkconfig.defaults'};{firmware_root / 'sdkconfig.artifact.defaults'}",
            }
        )
        subprocess.run(
            [idf_py, "-B", str(build_dir), "-D", f"SDKCONFIG={sdkconfig}", "build"],
            cwd=firmware_root,
            env=environment,
            check=True,
        )
        flasher_path = build_dir / "flasher_args.json"
        if not flasher_path.is_file():
            raise ArtifactError("ESP-IDF did not produce flasher_args.json")
        flasher = load_json_bytes(flasher_path.read_bytes(), "flasher_args.json")
        try:
            app_bin_relative = flasher["app"]["file"]
            bootloader_relative = flasher["bootloader"]["file"]
            partition_relative = flasher["partition-table"]["file"]
        except (KeyError, TypeError) as exc:
            raise ArtifactError("generated flash plan is missing required images") from exc
        app_bin = build_dir / app_bin_relative
        app_elf = app_bin.with_suffix(".elf")
        app_map = app_bin.with_suffix(".map")
        bootloader = build_dir / bootloader_relative
        partition_table = build_dir / partition_relative
        effective_sdkconfig = sdkconfig
        for required in (app_bin, app_elf, app_map, bootloader, partition_table, effective_sdkconfig):
            if required.is_symlink() or not required.is_file():
                raise ArtifactError("fresh build output is missing a required artifact")
        try:
            measure_and_enforce(
                app_bin,
                app_map,
                Path(idf_path) / "tools" / "idf_size.py",
            )
        except ResourceGateError as exc:
            raise ArtifactError(str(exc)) from exc

        bundle_name = f"{PROJECT}-firmware-{version}-{TARGET}-{profile}"
        staging = temporary_root / bundle_name
        _copy_regular(flasher_path, staging / "flasher_args.json")
        _copy_regular(app_bin, staging / app_bin_relative)
        _copy_regular(app_elf, staging / app_elf.relative_to(build_dir).as_posix())
        _copy_regular(bootloader, staging / bootloader_relative)
        _copy_regular(partition_table, staging / partition_relative)
        _copy_regular(effective_sdkconfig, staging / "provenance/sdkconfig")
        _copy_regular(source_root / "firmware/dependencies.lock", staging / "provenance/dependencies.lock")
        _copy_regular(source_root / "LICENSE", staging / "LICENSE")

        role_by_path = {
            "flasher_args.json": "flash_plan",
            app_bin_relative: "application_bin",
            app_elf.relative_to(build_dir).as_posix(): "application_elf",
            bootloader_relative: "bootloader_bin",
            partition_relative: "partition_table_bin",
            "provenance/sdkconfig": "effective_sdkconfig",
            "provenance/dependencies.lock": "dependencies_lock",
            "LICENSE": "license",
        }
        files: dict[str, dict[str, str]] = {
            path: {"sha256": sha256_file(staging / path), "role": role}
            for path, role in sorted(role_by_path.items())
        }
        manifest: dict[str, Any] = {
            "artifact_manifest_version": 1,
            "project": PROJECT,
            "firmware": {
                "version": version,
                "protocol_version": protocol_version,
                "source_revision": source_revision,
                "target": TARGET,
                "build_profile": profile,
                "idf_version": IDF_VERSION,
            },
            "runtime_identity": {"app_elf_sha256": files[next(path for path, role in role_by_path.items() if role == "application_elf")]["sha256"]},
            "build": {
                "reproducible": True,
                "source_date_epoch": source_date_epoch,
                "container_image": args.container_image,
                "tools": {
                    "compiler": _version_number([compiler, "--version"], "compiler"),
                    "cmake": _version_number(["cmake", "--version"], "CMake"),
                    "ninja": _version_number(["ninja", "--version"], "Ninja"),
                    "python": f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
                    "esptool": _version_number([sys.executable, "-m", "esptool", "version"], "esptool"),
                },
            },
            "provenance": {
                "dependencies_lock_sha256": files["provenance/dependencies.lock"]["sha256"],
                "effective_sdkconfig_sha256": files["provenance/sdkconfig"]["sha256"],
            },
            "flash_plan": "flasher_args.json",
            "files": files,
        }
        write_deterministic_json(staging / "manifest.json", manifest)
        checksum_paths = sorted(["manifest.json", *files])
        (staging / "SHA256SUMS").write_text(
            "".join(f"{sha256_file(staging / path)}  {path}\n" for path in checksum_paths),
            encoding="ascii",
        )
        verify_bundle_directory(staging)
        # The standalone verifier has generic privacy rules. The builder also
        # checks this invocation's machine-local values without recording them.
        check_bundle_privacy(staging, _privacy_markers(source_root))
        create_deterministic_tar_gz(staging, output, source_date_epoch)
        verify_bundle_archive(output)
    return output


def main() -> int:
    args = _parse_args()
    try:
        output = build(args)
    except (ArtifactError, OSError, subprocess.CalledProcessError) as exc:
        print(f"artifact build failed: {exc}", file=sys.stderr)
        return 1
    print(f"PASS: built and verified firmware artifact {output.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
