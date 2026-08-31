"""Non-destructive firmware staging and supported-device flash planning.

This module deliberately stops before any serial or subprocess operation.  It
creates one private snapshot, verifies that snapshot with the canonical
artifact verifier, and exposes only immutable values to a future flash
executor.
"""

from __future__ import annotations

import os
import shutil
import stat
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Any, Iterator, Mapping

from .artifact import (
    ArtifactError,
    _extract_archive_to,
    _role_paths,
    _verify_bundle_directory_with_plan,
    verify_bundle_directory,
)
from .firmware_verification import (
    ArtifactFirmwareIdentity,
    artifact_identity_from_verified_manifest,
)


SUPPORTED_PROFILE = "freenove-fnk0085"
SUPPORTED_TARGET = "esp32s3"
SUPPORTED_FLASH_MODE = "dio"
SUPPORTED_FLASH_SIZE = "4MB"
SUPPORTED_FLASH_FREQUENCY = "80m"
SUPPORTED_BEFORE_RESET = "default_reset"
SUPPORTED_AFTER_RESET = "hard_reset"
SUPPORTED_IMAGE_ORDER = (
    "bootloader_bin",
    "partition_table_bin",
    "application_bin",
)
SUPPORTED_OFFSETS = {
    "bootloader_bin": 0x0,
    "partition_table_bin": 0x8000,
    "application_bin": 0x10000,
}
REQUIRED_FLASH_SIZE_SETTING = "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y"
REQUIRED_NO_PSRAM_SETTING = "# CONFIG_SPIRAM is not set"


class ProvisioningPolicyError(ValueError):
    """A valid artifact is outside the supported B2 provisioning policy."""


@dataclass(frozen=True, slots=True)
class FlashImage:
    """One verified image in the private staging directory."""

    role: str
    offset: int
    path: Path
    encrypted: bool


@dataclass(frozen=True, slots=True)
class FlashPlan:
    """Structurally verified plan, normalized without mutable JSON values."""

    chip: str
    before_reset: str
    after_reset: str
    stub: bool
    flash_mode: str
    flash_size: str
    flash_freq: str
    images: tuple[FlashImage, ...]


@dataclass(frozen=True, slots=True)
class SupportedProvisioningPlan:
    """Plan that passed the explicit FNK0085 execution policy."""

    build_profile: str
    target: str
    flash_plan: FlashPlan
    sdkconfig_path: Path
    psram_required: bool = False


def _freeze_json(value: Any) -> Any:
    if isinstance(value, dict):
        return MappingProxyType({key: _freeze_json(item) for key, item in value.items()})
    if isinstance(value, list):
        return tuple(_freeze_json(item) for item in value)
    return value


@dataclass(frozen=True, slots=True)
class VerifiedFirmwareBundle:
    """Verified values whose paths are valid only inside the context.

    The private staging directory is removed by
    :func:`stage_and_verify_firmware_bundle` on every exit path.  The public
    manifest is recursively immutable and is not the execution interface;
    downstream code should consume ``provisioning_plan``.
    """

    staged_root: Path
    manifest: Mapping[str, Any]
    artifact_identity: ArtifactFirmwareIdentity
    flash_plan: FlashPlan
    provisioning_plan: SupportedProvisioningPlan
    _manifest_value: Mapping[str, Any] = field(repr=False, compare=False)

    def verify_staged_payloads_unchanged(self) -> None:
        """Recheck the same staged directory immediately before execution."""

        current = verify_bundle_directory(self.staged_root)
        if current != self._manifest_value:
            raise ArtifactError("verified staged artifact changed before execution")


def _source_stat(path: Path, description: str) -> os.stat_result:
    try:
        result = path.lstat()
    except OSError as exc:
        raise ArtifactError(f"could not read {description}") from exc
    if stat.S_ISLNK(result.st_mode):
        raise ArtifactError(f"symlinks are forbidden in {description}")
    return result


def _open_regular_snapshot(path: Path, description: str) -> tuple[int, os.stat_result]:
    original = _source_stat(path, description)
    if not stat.S_ISREG(original.st_mode) or original.st_nlink != 1:
        raise ArtifactError(f"{description} must be a single-link regular file")
    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise ArtifactError(f"could not open {description} for snapshot") from exc
    try:
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_nlink != 1
            or opened.st_dev != original.st_dev
            or opened.st_ino != original.st_ino
        ):
            raise ArtifactError(f"{description} changed during snapshot")
        if path.is_symlink():
            raise ArtifactError(f"symlinks are forbidden in {description}")
        return descriptor, opened
    except Exception:
        os.close(descriptor)
        raise


def _copy_regular_snapshot(source: Path, destination: Path, description: str) -> None:
    descriptor, _ = _open_regular_snapshot(source, description)
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            with os.fdopen(descriptor, "rb") as input_stream:
                descriptor = -1
                shutil.copyfileobj(input_stream, output)
    except (ArtifactError, OSError) as exc:
        if descriptor >= 0:
            os.close(descriptor)
        if isinstance(exc, ArtifactError):
            raise
        raise ArtifactError(f"could not snapshot {description}") from exc


def _copy_directory_snapshot(source: Path, destination: Path) -> None:
    root_stat = _source_stat(source, "artifact directory")
    if not stat.S_ISDIR(root_stat.st_mode):
        raise ArtifactError("artifact path must be a directory or archive")
    destination.mkdir(mode=0o700)

    def copy_tree(current_source: Path, current_destination: Path) -> None:
        try:
            entries = list(os.scandir(current_source))
        except OSError as exc:
            raise ArtifactError("could not read artifact directory") from exc
        for entry in entries:
            source_path = Path(entry.path)
            destination_path = current_destination / entry.name
            try:
                entry_stat = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise ArtifactError("could not inspect artifact directory entry") from exc
            if stat.S_ISLNK(entry_stat.st_mode):
                raise ArtifactError("symlinks are forbidden in artifact directory")
            if stat.S_ISDIR(entry_stat.st_mode):
                destination_path.mkdir(mode=0o700)
                copy_tree(source_path, destination_path)
                continue
            if stat.S_ISREG(entry_stat.st_mode):
                _copy_regular_snapshot(source_path, destination_path, "artifact file")
                continue
            raise ArtifactError("special files are forbidden in artifact directory")

    copy_tree(source, destination)


def _flash_plan_from_normalized(
    staged_root: Path, normalized: Mapping[str, Any]
) -> FlashPlan:
    raw_images = normalized["images"]
    images_by_role = {image["role"]: image for image in raw_images}
    if set(images_by_role) != set(SUPPORTED_IMAGE_ORDER):
        raise ProvisioningPolicyError("flash plan must contain the three supported image roles")
    images: list[FlashImage] = []
    for role in SUPPORTED_IMAGE_ORDER:
        image = images_by_role[role]
        relative = image["path"]
        path = staged_root / relative
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(staged_root.resolve())
        except (OSError, ValueError) as exc:
            raise ProvisioningPolicyError("flash image path escapes private staging") from exc
        value = image["encrypted"]
        if type(value) is bool:
            encrypted = value
        elif type(value) is str and value == "false":
            encrypted = False
        else:
            raise ProvisioningPolicyError("B2 supports only explicitly unencrypted images")
        images.append(
            FlashImage(
                role=role,
                offset=int(image["offset"]),
                path=resolved,
                encrypted=encrypted,
            )
        )
    return FlashPlan(
        chip=normalized["chip"],
        before_reset=normalized["before"],
        after_reset=normalized["after"],
        stub=normalized["stub"],
        flash_mode=normalized["flash_mode"],
        flash_size=normalized["flash_size"],
        flash_freq=normalized["flash_freq"],
        images=tuple(images),
    )


def _memory_profile(staged_root: Path, manifest: Mapping[str, Any]) -> Path:
    paths = _role_paths(manifest)
    sdkconfig_relative = next(
        path for path, (_, role) in paths.items() if role == "effective_sdkconfig"
    )
    sdkconfig = staged_root / sdkconfig_relative
    try:
        lines = set(sdkconfig.read_text(encoding="utf-8", errors="strict").splitlines())
    except (OSError, UnicodeError) as exc:
        raise ProvisioningPolicyError("verified effective sdkconfig cannot be read") from exc
    enabled_flash_sizes = {
        line
        for line in lines
        if line.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_") and line.endswith("=y")
    }
    if enabled_flash_sizes != {REQUIRED_FLASH_SIZE_SETTING}:
        raise ProvisioningPolicyError("firmware memory profile is not the supported 4MB setting")
    if REQUIRED_NO_PSRAM_SETTING not in lines or "CONFIG_SPIRAM=y" in lines:
        raise ProvisioningPolicyError("firmware requires external PSRAM")
    return sdkconfig


def _supported_plan(
    staged_root: Path,
    manifest: Mapping[str, Any],
    normalized: Mapping[str, Any],
) -> tuple[FlashPlan, SupportedProvisioningPlan]:
    firmware = manifest["firmware"]
    if firmware["build_profile"] != SUPPORTED_PROFILE:
        raise ProvisioningPolicyError("artifact profile is not supported for this fixture")
    if firmware["target"] != SUPPORTED_TARGET or normalized["chip"] != SUPPORTED_TARGET:
        raise ProvisioningPolicyError("artifact target is not supported for this fixture")
    if normalized["before"] != SUPPORTED_BEFORE_RESET or normalized["after"] != SUPPORTED_AFTER_RESET:
        raise ProvisioningPolicyError("flash reset policy is not supported")
    if normalized["stub"] is not True:
        raise ProvisioningPolicyError("B2 requires the esptool stub loader")
    if (
        normalized["flash_mode"] != SUPPORTED_FLASH_MODE
        or normalized["flash_size"] != SUPPORTED_FLASH_SIZE
        or normalized["flash_freq"] != SUPPORTED_FLASH_FREQUENCY
    ):
        raise ProvisioningPolicyError("flash settings are outside the supported policy")
    plan = _flash_plan_from_normalized(staged_root, normalized)
    if len(plan.images) != 3:
        raise ProvisioningPolicyError("exactly three flash images are required")
    for image in plan.images:
        if image.offset != SUPPORTED_OFFSETS[image.role]:
            raise ProvisioningPolicyError("flash image offset is outside the supported policy")
        if image.encrypted:
            raise ProvisioningPolicyError("encrypted images are not supported by B2")
    sdkconfig_path = _memory_profile(staged_root, manifest)
    approved = SupportedProvisioningPlan(
        build_profile=SUPPORTED_PROFILE,
        target=SUPPORTED_TARGET,
        flash_plan=plan,
        sdkconfig_path=sdkconfig_path,
        psram_required=False,
    )
    return plan, approved


@contextmanager
def stage_and_verify_firmware_bundle(
    artifact: str | os.PathLike[str] | Path,
) -> Iterator[VerifiedFirmwareBundle]:
    """Snapshot, verify, and policy-check an artifact for B2 execution.

    The yielded bundle owns a private staging directory for the duration of
    the context.  No serial port, esptool import, subprocess, or hardware
    access occurs here.
    """

    source = Path(artifact)
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-provision-") as temporary:
        root = Path(temporary)
        source_stat = _source_stat(source, "artifact input")
        if stat.S_ISREG(source_stat.st_mode):
            snapshot = root / "input.tar.gz"
            _copy_regular_snapshot(source, snapshot, "artifact archive")
            staged_root = _extract_archive_to(snapshot, root / "archive")
        elif stat.S_ISDIR(source_stat.st_mode):
            staged_root = root / "bundle"
            _copy_directory_snapshot(source, staged_root)
        else:
            raise ArtifactError("artifact path must be a directory or archive")
        manifest, normalized = _verify_bundle_directory_with_plan(staged_root)
        flash_plan, provisioning_plan = _supported_plan(staged_root, manifest, normalized)
        identity = artifact_identity_from_verified_manifest(manifest)
        frozen_manifest = _freeze_json(manifest)
        assert isinstance(frozen_manifest, Mapping)
        bundle = VerifiedFirmwareBundle(
            staged_root=staged_root,
            manifest=frozen_manifest,
            artifact_identity=identity,
            flash_plan=flash_plan,
            provisioning_plan=provisioning_plan,
            _manifest_value=manifest,
        )
        yield bundle


def plan_esptool_v4_args(
    plan: SupportedProvisioningPlan,
    port: str,
) -> tuple[str, ...]:
    """Create pure esptool-v4 arguments after ``python -m esptool``."""

    if not isinstance(plan, SupportedProvisioningPlan):
        raise TypeError("an approved SupportedProvisioningPlan is required")
    if not isinstance(port, str) or not port or "\x00" in port:
        raise ValueError("a non-empty serial port is required")
    flash = plan.flash_plan
    if tuple(image.role for image in flash.images) != SUPPORTED_IMAGE_ORDER:
        raise ProvisioningPolicyError("approved image order is invalid")
    arguments: list[str] = [
        "--chip",
        plan.target,
        "--port",
        port,
        "--before",
        flash.before_reset,
        "--after",
        flash.after_reset,
        "write_flash",
        "--flash_mode",
        flash.flash_mode,
        "--flash_size",
        flash.flash_size,
        "--flash_freq",
        flash.flash_freq,
    ]
    for image in flash.images:
        arguments.extend((hex(image.offset), str(image.path)))
    return tuple(arguments)
