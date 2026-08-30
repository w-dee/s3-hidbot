#!/usr/bin/env python3
"""Shared manifest, bundle, privacy, and deterministic archive primitives.

This module deliberately has no repository or Git dependency.  It is usable by
the builder and by the verifier copied into a source-independent consumer
workflow.
"""

from __future__ import annotations

import copy
import gzip
import hashlib
import io
import json
import re
import shutil
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Mapping


SCHEMA_VERSION = 1
PROJECT = "s3-hidbot"
TARGET = "esp32s3"
IDF_VERSION = "v5.5.4"
PROTOCOL_VERSION = 1
MAX_SOURCE_DATE_EPOCH = 4_102_444_800  # 2100-01-01T00:00:00Z

_SEMVER = re.compile(
    r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
    r"(?:-(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?\Z"
)
_HEX64 = re.compile(r"[0-9a-f]{64}\Z")
_REVISION40 = re.compile(r"[0-9a-f]{40}\Z")
_PROFILE = re.compile(r"[a-z0-9][a-z0-9-]{0,30}\Z")
_TOOL_VERSION = re.compile(
    r"[0-9]+\.[0-9]+(?:\.[0-9]+){0,3}"
    r"(?:(?:\.(?:dev|post)[0-9]+)|(?:a|b|rc)[0-9]+)?\Z"
)
_MAX_TOOL_VERSION_LENGTH = 31
_LINUX_ROOT = b"/" + b"home" + b"/"
_MAC_ROOT = b"/" + b"Users" + b"/"
_LINUX_HOME = re.compile(_LINUX_ROOT + rb"(?!USER/|<user>/)[^/\r\n]+/")
_MAC_HOME = re.compile(_MAC_ROOT + rb"(?!USER/|<user>/)[^/\r\n]+/")
_WINDOWS_HOME = re.compile(
    rb"[A-Za-z]:" + re.escape(b"\\") + b"Users" + re.escape(b"\\")
    + rb"(?!USER\\|<user>\\)[^\\\r\n]+\\"
)
_FORBIDDEN_PAYLOAD_MARKERS = (
    b"/dev/serial/",
    b"S3_HIDBOT_SERIAL",
    b".envrc",
    b"PRIVATE KEY",
    b"GITHUB_TOKEN",
)

ROLES = frozenset(
    {
        "application_elf",
        "application_bin",
        "bootloader_bin",
        "partition_table_bin",
        "flash_plan",
        "effective_sdkconfig",
        "dependencies_lock",
        "license",
    }
)
REQUIRED_ROLES = ROLES
PLAN_ROLE_BY_KEY = {
    "bootloader": "bootloader_bin",
    "app": "application_bin",
    "partition-table": "partition_table_bin",
}


class ArtifactError(ValueError):
    """Raised for any malformed, unsafe, or unverifiable artifact."""


def _error(message: str) -> ArtifactError:
    return ArtifactError(message)


def validate_source_revision(value: Any) -> str:
    if not isinstance(value, str) or _REVISION40.fullmatch(value) is None:
        raise _error("source_revision must be exactly 40 lowercase hexadecimal characters")
    return value


def validate_source_date_epoch(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _error("source_date_epoch must be an integer")
    if value < 0 or value > MAX_SOURCE_DATE_EPOCH:
        raise _error("source_date_epoch is outside the supported range")
    return value


def validate_relative_path(value: Any) -> str:
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise _error("bundle paths must be non-empty POSIX strings")
    if value.startswith("/") or re.match(r"^[A-Za-z]:", value):
        raise _error(f"absolute bundle path is forbidden: {value!r}")
    parts = value.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise _error(f"unsafe bundle path: {value!r}")
    return value


def validate_hash(value: Any) -> str:
    if not isinstance(value, str) or _HEX64.fullmatch(value) is None:
        raise _error("SHA-256 values must be lowercase hexadecimal length 64")
    return value


def validate_version(value: Any) -> str:
    if not isinstance(value, str) or _SEMVER.fullmatch(value) is None or len(value) > 31:
        raise _error("firmware version is not a supported semantic version")
    return value


def validate_profile(value: Any) -> str:
    if not isinstance(value, str) or _PROFILE.fullmatch(value) is None:
        raise _error("build profile is invalid")
    return value


def validate_tool_version(value: Any) -> str:
    if (
        not isinstance(value, str)
        or len(value) > _MAX_TOOL_VERSION_LENGTH
        or _TOOL_VERSION.fullmatch(value) is None
    ):
        raise _error("tool version must be a normalized constrained version")
    return value


def _strict_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _error(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json_bytes(payload: bytes, description: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"), object_pairs_hook=_strict_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError, ArtifactError) as exc:
        raise _error(f"invalid {description} JSON") from exc
    if not isinstance(value, dict):
        raise _error(f"{description} must be a JSON object")
    return value


def write_deterministic_json(path: Path, value: Mapping[str, Any]) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True, separators=(",", ": "))
        + "\n",
        encoding="utf-8",
    )


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_keys(value: Mapping[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise _error(f"{label} fields are not exactly the manifest schema")


def _role_paths(manifest: Mapping[str, Any]) -> dict[str, tuple[str, str]]:
    files = manifest["files"]
    if not isinstance(files, dict) or not files:
        raise _error("manifest files must be a non-empty object")
    paths: dict[str, tuple[str, str]] = {}
    roles: dict[str, str] = {}
    for path, entry in files.items():
        validate_relative_path(path)
        if path in {"manifest.json", "SHA256SUMS"}:
            raise _error("manifest and checksum files cannot be payload entries")
        if not isinstance(entry, dict):
            raise _error("manifest file entries must be objects")
        _require_keys(entry, {"sha256", "role"}, f"file {path}")
        digest = validate_hash(entry["sha256"])
        role = entry["role"]
        if not isinstance(role, str) or role not in ROLES:
            raise _error(f"unknown artifact file role: {role!r}")
        if role in roles:
            raise _error(f"duplicate artifact file role: {role}")
        paths[path] = (digest, role)
        roles[role] = path
    if set(roles) != REQUIRED_ROLES:
        raise _error("manifest does not contain exactly the required payload roles")
    return paths


def _validate_manifest(manifest: Mapping[str, Any]) -> dict[str, tuple[str, str]]:
    _require_keys(
        manifest,
        {
            "artifact_manifest_version",
            "project",
            "firmware",
            "runtime_identity",
            "build",
            "provenance",
            "flash_plan",
            "files",
        },
        "manifest",
    )
    if manifest["artifact_manifest_version"] != SCHEMA_VERSION:
        raise _error("unsupported artifact manifest version")
    if manifest["project"] != PROJECT:
        raise _error("artifact project is incompatible")

    firmware = manifest["firmware"]
    if not isinstance(firmware, dict):
        raise _error("firmware manifest section must be an object")
    _require_keys(
        firmware,
        {"version", "protocol_version", "source_revision", "target", "build_profile", "idf_version"},
        "firmware",
    )
    validate_version(firmware["version"])
    if firmware["protocol_version"] != PROTOCOL_VERSION:
        raise _error("unsupported protocol version")
    validate_source_revision(firmware["source_revision"])
    if firmware["target"] != TARGET or firmware["idf_version"] != IDF_VERSION:
        raise _error("firmware target or IDF version is incompatible")
    validate_profile(firmware["build_profile"])

    identity = manifest["runtime_identity"]
    if not isinstance(identity, dict):
        raise _error("runtime_identity must be an object")
    _require_keys(identity, {"app_elf_sha256"}, "runtime_identity")
    runtime_elf_hash = validate_hash(identity["app_elf_sha256"])

    build = manifest["build"]
    if not isinstance(build, dict):
        raise _error("build must be an object")
    _require_keys(build, {"reproducible", "source_date_epoch", "container_image", "tools"}, "build")
    if build["reproducible"] is not True:
        raise _error("artifact build must be reproducible")
    validate_source_date_epoch(build["source_date_epoch"])
    container = build["container_image"]
    if container is not None and (
        not isinstance(container, str)
        or re.fullmatch(r"[^\s@]+@sha256:[0-9a-f]{64}", container) is None
    ):
        raise _error("container_image must be null or an immutable digest reference")
    tools = build["tools"]
    if not isinstance(tools, dict):
        raise _error("build.tools must be an object")
    _require_keys(tools, {"compiler", "cmake", "ninja", "python", "esptool"}, "build.tools")
    for value in tools.values():
        validate_tool_version(value)

    provenance = manifest["provenance"]
    if not isinstance(provenance, dict):
        raise _error("provenance must be an object")
    _require_keys(provenance, {"dependencies_lock_sha256", "effective_sdkconfig_sha256"}, "provenance")
    validate_hash(provenance["dependencies_lock_sha256"])
    validate_hash(provenance["effective_sdkconfig_sha256"])

    validate_relative_path(manifest["flash_plan"])
    paths = _role_paths(manifest)
    elf_path = next(path for path, (_, role) in paths.items() if role == "application_elf")
    if paths[elf_path][0] != runtime_elf_hash:
        raise _error("runtime ELF identity does not match the application ELF payload")
    for role, field in (
        ("dependencies_lock", "dependencies_lock_sha256"),
        ("effective_sdkconfig", "effective_sdkconfig_sha256"),
    ):
        path = next(path for path, (_, candidate) in paths.items() if candidate == role)
        if paths[path][0] != provenance[field]:
            raise _error(f"provenance hash does not match {role} payload")
    plan_path = manifest["flash_plan"]
    if paths.get(plan_path, (None, None))[1] != "flash_plan":
        raise _error("flash_plan must point to the flash_plan file role")
    return paths


def _validate_flash_plan(bundle: Path, manifest: Mapping[str, Any], paths: Mapping[str, tuple[str, str]]) -> None:
    plan_path = bundle / manifest["flash_plan"]
    plan = load_json_bytes(plan_path.read_bytes(), "flasher_args.json")
    _require_keys(
        plan,
        {"write_flash_args", "flash_settings", "flash_files", "bootloader", "app", "partition-table", "extra_esptool_args"},
        "flasher_args.json",
    )
    settings = plan["flash_settings"]
    if not isinstance(settings, dict):
        raise _error("flash_settings must be an object")
    _require_keys(settings, {"flash_mode", "flash_size", "flash_freq"}, "flash_settings")
    if not all(isinstance(value, str) and value for value in settings.values()):
        raise _error("flash settings must be non-empty strings")
    write_args = plan["write_flash_args"]
    if not isinstance(write_args, list) or len(write_args) % 2:
        raise _error("write_flash_args must contain flag/value pairs")
    seen_settings: dict[str, str] = {}
    for index in range(0, len(write_args), 2):
        flag, value = write_args[index : index + 2]
        if flag not in {"--flash_mode", "--flash_size", "--flash_freq"} or not isinstance(value, str):
            raise _error("unexpected write_flash_args value")
        if flag in seen_settings:
            raise _error("duplicate write_flash_args setting")
        seen_settings[flag] = value
    if seen_settings != {
        "--flash_mode": settings["flash_mode"],
        "--flash_size": settings["flash_size"],
        "--flash_freq": settings["flash_freq"],
    }:
        raise _error("write_flash_args and flash_settings disagree")

    extra = plan["extra_esptool_args"]
    if not isinstance(extra, dict):
        raise _error("extra_esptool_args must be an object")
    _require_keys(extra, {"before", "after", "stub", "chip"}, "extra_esptool_args")
    if extra["chip"] != TARGET or not isinstance(extra["stub"], bool):
        raise _error("flash plan chip or stub policy is invalid")
    if not all(isinstance(extra[key], str) and extra[key] for key in ("before", "after")):
        raise _error("flash reset policy is invalid")

    plan_files = plan["flash_files"]
    if not isinstance(plan_files, dict) or len(plan_files) != len(PLAN_ROLE_BY_KEY):
        raise _error("flash_files must contain the three generated images")
    numeric_offsets: set[int] = set()
    for offset, path in plan_files.items():
        if not isinstance(offset, str):
            raise _error("flash offsets must be strings")
        try:
            numeric = int(offset, 0)
        except ValueError as exc:
            raise _error("flash offset is not numeric") from exc
        if numeric < 0 or numeric > 0xFFFFFFFF or numeric in numeric_offsets:
            raise _error("flash offsets are invalid or duplicated")
        numeric_offsets.add(numeric)
        validate_relative_path(path)
        if path not in paths or paths[path][1] not in PLAN_ROLE_BY_KEY.values():
            raise _error("flash plan references an unlisted payload")
    for key, role in PLAN_ROLE_BY_KEY.items():
        entry = plan[key]
        if not isinstance(entry, dict):
            raise _error(f"flash plan {key} entry must be an object")
        _require_keys(entry, {"offset", "file", "encrypted"}, f"flash plan {key}")
        try:
            numeric = int(entry["offset"], 0)
        except (TypeError, ValueError) as exc:
            raise _error(f"flash plan {key} offset is invalid") from exc
        if numeric not in numeric_offsets:
            raise _error(f"flash plan {key} offset is absent from flash_files")
        path = entry["file"]
        validate_relative_path(path)
        if path not in paths or paths[path][1] != role or plan_files.get(entry["offset"]) != path:
            raise _error(f"flash plan {key} mapping does not match its manifest role")
        encrypted = entry["encrypted"]
        if encrypted not in {True, False, "true", "false"}:
            raise _error(f"flash plan {key} encryption value is invalid")


def _iter_payload_files(bundle: Path) -> list[tuple[str, Path]]:
    if bundle.is_symlink() or not bundle.is_dir():
        raise _error("bundle path must be a directory")
    files: list[tuple[str, Path]] = []
    for path in bundle.rglob("*"):
        if path.is_symlink():
            raise _error("symlinks are forbidden in an artifact bundle")
        if path.is_file():
            relative = path.relative_to(bundle).as_posix()
            validate_relative_path(relative)
            files.append((relative, path))
        elif not path.is_dir():
            raise _error("special files are forbidden in an artifact bundle")
    return files


def _validate_sha256sums(bundle: Path, expected: Mapping[str, tuple[str, str]]) -> None:
    checksum_path = bundle / "SHA256SUMS"
    if checksum_path.is_symlink() or not checksum_path.is_file():
        raise _error("SHA256SUMS is missing")
    actual: dict[str, str] = {}
    try:
        lines = checksum_path.read_text(encoding="ascii").splitlines()
    except (UnicodeDecodeError, OSError) as exc:
        raise _error("SHA256SUMS is not ASCII") from exc
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\r\n]+)", line)
        if match is None:
            raise _error("malformed SHA256SUMS line")
        digest, path = match.groups()
        validate_relative_path(path)
        if path in actual:
            raise _error("duplicate SHA256SUMS path")
        actual[path] = digest
    expected_sums = {"manifest.json": sha256_file(bundle / "manifest.json")}
    expected_sums.update({path: digest for path, (digest, _) in expected.items()})
    if actual != expected_sums:
        raise _error("SHA256SUMS does not exactly cover the manifest and payload")
    if list(actual) != sorted(expected_sums):
        raise _error("SHA256SUMS paths are not in lexical order")
    for path, digest in actual.items():
        if sha256_file(bundle / path) != digest:
            raise _error(f"SHA256SUMS hash mismatch: {path}")


def check_bundle_privacy(bundle: Path, extra_markers: tuple[bytes, ...] = ()) -> None:
    markers = _FORBIDDEN_PAYLOAD_MARKERS + tuple(marker for marker in extra_markers if marker)
    for relative, path in _iter_payload_files(bundle):
        payload = path.read_bytes()
        if any(pattern.search(payload) for pattern in (_LINUX_HOME, _MAC_HOME, _WINDOWS_HOME)):
            raise _error(f"developer-specific absolute path in payload: {relative}")
        if any(marker in payload for marker in markers):
            raise _error(f"machine-local or secret marker in payload: {relative}")


def verify_bundle_directory(bundle: Path) -> dict[str, Any]:
    manifest_path = bundle / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise _error("manifest.json is missing")
    manifest = load_json_bytes(manifest_path.read_bytes(), "manifest")
    paths = _validate_manifest(manifest)
    disk_files = {relative for relative, _ in _iter_payload_files(bundle)}
    expected_files = set(paths) | {"manifest.json", "SHA256SUMS"}
    if disk_files != expected_files:
        raise _error("bundle contains missing or unexpected files")
    for relative, (expected_hash, _) in paths.items():
        if sha256_file(bundle / relative) != expected_hash:
            raise _error(f"payload hash mismatch: {relative}")
    sdkconfig_path = next(path for path, (_, role) in paths.items() if role == "effective_sdkconfig")
    sdkconfig_lines = (bundle / sdkconfig_path).read_text(encoding="utf-8", errors="strict").splitlines()
    if "CONFIG_APP_REPRODUCIBLE_BUILD=y" not in sdkconfig_lines:
        raise _error("effective sdkconfig does not enable reproducible builds")
    _validate_sha256sums(bundle, paths)
    _validate_flash_plan(bundle, manifest, paths)
    check_bundle_privacy(bundle)
    license_path = next(path for path, (_, role) in paths.items() if role == "license")
    if b"MIT License" not in (bundle / license_path).read_bytes():
        raise _error("license payload is not the expected MIT license")
    return copy.deepcopy(manifest)


def _safe_archive_member_name(name: str) -> str:
    # Tar directory entries conventionally end in '/', which is not a bundle
    # path component. Normalize only that final separator before validation.
    normalized = name[:-1] if name.endswith("/") else name
    return validate_relative_path(normalized)


def verify_bundle_archive(archive: Path) -> dict[str, Any]:
    if archive.is_symlink() or not archive.is_file():
        raise _error("artifact archive is missing")
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-artifact-verify-") as temporary:
        extraction_root = Path(temporary)
        try:
            with tarfile.open(archive, mode="r:gz") as source:
                members = source.getmembers()
                if not members:
                    raise _error("artifact archive is empty")
                top_level: str | None = None
                names: set[str] = set()
                for member in members:
                    name = _safe_archive_member_name(member.name)
                    if name in names:
                        raise _error("duplicate archive member")
                    names.add(name)
                    first = name.split("/", 1)[0]
                    if top_level is None:
                        top_level = first
                    elif first != top_level:
                        raise _error("archive must contain one top-level directory")
                    if not (member.isdir() or member.isfile()):
                        raise _error("archive symlink or special member is forbidden")
                    destination = extraction_root.joinpath(*name.split("/"))
                    if member.isdir():
                        destination.mkdir(parents=True, exist_ok=True)
                        continue
                    extracted = source.extractfile(member)
                    if extracted is None:
                        raise _error("archive member cannot be read")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(extracted.read())
        except (tarfile.TarError, OSError) as exc:
            if isinstance(exc, ArtifactError):
                raise
            raise _error("could not safely read artifact archive") from exc
        if top_level is None:
            raise _error("archive has no top-level directory")
        return verify_bundle_directory(extraction_root / top_level)


def _tar_info(name: str, *, is_directory: bool, size: int, source_date_epoch: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.mode = 0o755 if is_directory else 0o644
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = source_date_epoch
    info.type = tarfile.DIRTYPE if is_directory else tarfile.REGTYPE
    info.size = 0 if is_directory else size
    return info


def create_deterministic_tar_gz(bundle: Path, archive: Path, source_date_epoch: int) -> None:
    validate_source_date_epoch(source_date_epoch)
    if bundle.is_symlink() or not bundle.is_dir():
        raise _error("bundle path must be a directory")
    archive.parent.mkdir(parents=True, exist_ok=True)
    root_name = validate_relative_path(bundle.name)
    members: list[tuple[str, Path | None, bool]] = [(root_name, None, True)]
    entries = sorted(bundle.rglob("*"), key=lambda path: path.relative_to(bundle).as_posix())
    seen_dirs: set[str] = set()
    for entry in entries:
        relative = entry.relative_to(bundle).as_posix()
        validate_relative_path(relative)
        if entry.is_symlink():
            raise _error("symlinks are forbidden in deterministic archives")
        archive_name = f"{root_name}/{relative}"
        if entry.is_dir():
            members.append((archive_name, None, True))
            seen_dirs.add(archive_name)
        elif entry.is_file():
            parent = PurePosixPath(archive_name).parent
            parent_parts: list[str] = []
            while str(parent) != root_name:
                parent_parts.append(str(parent))
                parent = parent.parent
            for parent_name in reversed(parent_parts):
                if parent_name not in seen_dirs:
                    members.append((parent_name, None, True))
                    seen_dirs.add(parent_name)
            members.append((archive_name, entry, False))
        else:
            raise _error("special files are forbidden in deterministic archives")
    members = sorted(members, key=lambda item: item[0])
    with archive.open("wb") as output:
        with gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=source_date_epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                for name, source, is_directory in members:
                    if is_directory:
                        tar.addfile(_tar_info(name, is_directory=True, size=0, source_date_epoch=source_date_epoch))
                    else:
                        assert source is not None
                        payload = source.read_bytes()
                        info = _tar_info(
                            name,
                            is_directory=False,
                            size=len(payload),
                            source_date_epoch=source_date_epoch,
                        )
                        tar.addfile(info, io.BytesIO(payload))
