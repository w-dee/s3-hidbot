"""Source and verified firmware-artifact identity for qualification evidence."""

from __future__ import annotations

import hashlib
import struct
import subprocess
from collections.abc import Callable, Mapping
from contextlib import AbstractContextManager
from pathlib import Path
from typing import Any

from .core import QualificationError


RunGit = Callable[[list[str], Path], str]
BundleLoader = Callable[[Path], AbstractContextManager[Any]]


def _run_git(arguments: list[str], repository: Path) -> str:
    result = subprocess.run(
        arguments,
        cwd=repository,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def derive_source_identity(
    repository: Path | str, *, run: RunGit = _run_git
) -> dict[str, Any]:
    """Derive source identity from Git at invocation time."""

    root = Path(repository)
    revision = run(["git", "rev-parse", "HEAD"], root).strip()
    branch = run(["git", "branch", "--show-current"], root).strip()
    porcelain = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], root
    )
    if len(revision) != 40 or any(character not in "0123456789abcdef" for character in revision):
        raise QualificationError("Git returned an invalid source revision")
    if not branch:
        branch = "DETACHED"
    return {"revision": revision, "branch": branch, "dirty": bool(porcelain)}


def _default_bundle_loader(path: Path) -> AbstractContextManager[Any]:
    # This is the repository's authoritative artifact verifier and policy
    # checker. Import lazily so pure harness tests need no host dependencies.
    from hidbot.provisioning import stage_and_verify_firmware_bundle

    return stage_and_verify_firmware_bundle(path)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_partition_geometry(payload: bytes) -> list[dict[str, Any]]:
    """Parse the verified ESP-IDF binary partition table, excluding MD5 data."""

    entries: list[dict[str, Any]] = []
    for cursor in range(0, len(payload), 32):
        record = payload[cursor : cursor + 32]
        if len(record) < 32:
            raise QualificationError("partition table has a truncated entry")
        if record == b"\xff" * 32 or record[:2] == b"\xeb\xeb":
            break
        try:
            magic, part_type, subtype, offset, size, raw_label, flags = struct.unpack(
                "<2sBBLL16sL", record
            )
        except struct.error as exc:
            raise QualificationError("partition table entry is malformed") from exc
        if magic != b"\xaa\x50":
            raise QualificationError("partition table contains an unknown entry")
        try:
            label = raw_label.split(b"\x00", 1)[0].decode("ascii")
        except UnicodeDecodeError as exc:
            raise QualificationError("partition label is not ASCII") from exc
        if not label:
            raise QualificationError("partition label is empty")
        entries.append(
            {
                "label": label,
                "type": part_type,
                "subtype": subtype,
                "offset": offset,
                "size": size,
                "encrypted": bool(flags & 1),
                "readonly": bool(flags & 2),
            }
        )
    if not entries:
        raise QualificationError("partition table contains no entries")
    return entries


def _role_hashes(manifest: Mapping[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in manifest["files"].values():
        result[str(entry["role"])] = str(entry["sha256"])
    return dict(sorted(result.items()))


def artifact_preflight(
    artifact: Path | str,
    *,
    source: Mapping[str, Any] | None = None,
    loader: BundleLoader = _default_bundle_loader,
) -> dict[str, Any]:
    """Run official verification and return path-free artifact evidence."""

    path = Path(artifact)
    archive_sha256 = _sha256_file(path) if path.is_file() else None
    with loader(path) as bundle:
        bundle.verify_staged_payloads_unchanged()
        manifest = bundle.manifest
        firmware = manifest["firmware"]
        role_hashes = _role_hashes(manifest)
        images = [
            {
                "role": image.role,
                "offset": image.offset,
                "sha256": role_hashes[image.role],
                "encrypted": image.encrypted,
            }
            for image in bundle.flash_plan.images
        ]
        flash_payloads = {
            image["role"]: image["sha256"] for image in images
        }
        partition_image = next(
            image for image in bundle.flash_plan.images if image.role == "partition_table_bin"
        )
        result: dict[str, Any] = {
            "verification": "VALID",
            "archive_sha256": archive_sha256,
            "manifest_version": manifest["artifact_manifest_version"],
            "project": manifest["project"],
            "firmware": {
                "version": firmware["version"],
                "protocol_version": firmware["protocol_version"],
                "source_revision": firmware["source_revision"],
                "target": firmware["target"],
                "build_profile": firmware["build_profile"],
                "idf_version": firmware["idf_version"],
            },
            "runtime_elf_sha256": manifest["runtime_identity"]["app_elf_sha256"],
            "payloads": role_hashes,
            "flash_payloads": flash_payloads,
            "flash": {
                "chip": bundle.flash_plan.chip,
                "before": bundle.flash_plan.before_reset,
                "after": bundle.flash_plan.after_reset,
                "stub": bundle.flash_plan.stub,
                "mode": bundle.flash_plan.flash_mode,
                "size": bundle.flash_plan.flash_size,
                "frequency": bundle.flash_plan.flash_freq,
                "images": images,
            },
            "partitions": parse_partition_geometry(partition_image.path.read_bytes()),
        }
    if source is not None and source.get("revision") != result["firmware"]["source_revision"]:
        raise QualificationError("artifact source revision does not match repository HEAD")
    return result


def _same(left: Mapping[str, Any], right: Mapping[str, Any], key: str) -> bool:
    return left.get(key) == right.get(key)


def _valid_hash(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _flash_semantics(value: Mapping[str, Any]) -> dict[str, Any]:
    flash = value.get("flash", {})
    if not isinstance(flash, Mapping):
        return {}
    images = flash.get("images", [])
    if not isinstance(images, list):
        images = []
    return {
        key: flash.get(key)
        for key in ("chip", "before", "after", "stub", "mode", "size", "frequency")
    } | {
        "images": [
            {
                "role": image.get("role"),
                "offset": image.get("offset"),
                "encrypted": image.get("encrypted"),
            }
            for image in images
            if isinstance(image, Mapping)
        ]
    }


def _valid_flash(value: Any) -> bool:
    if not isinstance(value, Mapping):
        return False
    scalar_fields = ("chip", "before", "after", "mode", "size", "frequency")
    if any(not isinstance(value.get(field), str) or not value[field] for field in scalar_fields):
        return False
    if type(value.get("stub")) is not bool:
        return False
    images = value.get("images")
    if not isinstance(images, list) or len(images) != 3:
        return False
    roles: set[str] = set()
    offsets: set[int] = set()
    for image in images:
        if not isinstance(image, Mapping):
            return False
        role, offset, encrypted = (
            image.get("role"), image.get("offset"), image.get("encrypted")
        )
        if not isinstance(role, str) or type(offset) is not int or offset < 0:
            return False
        if type(encrypted) is not bool:
            return False
        roles.add(role)
        offsets.add(offset)
    return (
        roles == {"application_bin", "bootloader_bin", "partition_table_bin"}
        and len(offsets) == 3
    )


def _source_revision(value: Mapping[str, Any]) -> Any:
    firmware = value.get("firmware")
    return firmware.get("source_revision") if isinstance(firmware, Mapping) else None


def compare_artifact_identity(
    left: Mapping[str, Any], right: Mapping[str, Any]
) -> dict[str, bool]:
    """Compare archive provenance separately from runtime qualification identity."""

    left_archive = left.get("archive_sha256")
    archive = _valid_hash(left_archive) and left_archive == right.get("archive_sha256")
    left_payloads = left.get("flash_payloads")
    payloads = (
        isinstance(left_payloads, Mapping)
        and set(left_payloads) == {
            "application_bin", "bootloader_bin", "partition_table_bin"
        }
        and all(_valid_hash(digest) for digest in left_payloads.values())
        and left_payloads == right.get("flash_payloads")
    )
    left_source = _source_revision(left)
    source = (
        isinstance(left_source, str)
        and len(left_source) == 40
        and all(character in "0123456789abcdef" for character in left_source)
        and left_source == _source_revision(right)
    )
    left_runtime = left.get("runtime_elf_sha256")
    runtime = (
        _valid_hash(left_runtime)
        and left_runtime == right.get("runtime_elf_sha256")
    )
    left_flash = left.get("flash")
    left_partitions = left.get("partitions")
    flash = (
        _valid_flash(left_flash)
        and isinstance(left_partitions, list)
        and bool(left_partitions)
        and _flash_semantics(left) == _flash_semantics(right)
        and _same(left, right, "partitions")
    )
    return {
        "archive_byte_identity": archive,
        "flash_payload_identity": payloads,
        "source_sha_identity": source,
        "runtime_elf_identity": runtime,
        "flash_semantics_identity": flash,
        "physical_qualification_carry_forward": payloads and source and runtime and flash,
    }
