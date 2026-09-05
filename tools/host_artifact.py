"""Validation for the U6.4A canonical downloadable host wheel."""

from __future__ import annotations

import hashlib
import re
import tomllib
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


DISTRIBUTION_NAME = "s3-hidbot-host"
_VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
REQUIRED_MODULES = frozenset(
    {
        "hidbot/__init__.py",
        "hidbot/artifact.py",
        "hidbot/cli.py",
        "hidbot/client.py",
        "hidbot/errors.py",
        "hidbot/firmware_verification.py",
        "hidbot/flashing.py",
        "hidbot/framing.py",
        "hidbot/protocol.py",
        "hidbot/provisioning.py",
        "hidbot/provisioning_workflow.py",
        "hidbot/serial_transport.py",
    }
)
_CHECKSUM = re.compile(r"^[0-9a-f]{64}  ([A-Za-z0-9][A-Za-z0-9_.-]*\.whl)\n$")
_LINUX_HOME_ROOT = rb"/" + rb"home/"
_MACOS_HOME_ROOT = rb"/" + rb"Users/"
_WINDOWS_HOME_ROOT = rb"[A-Za-z]:\\" + rb"Users\\"
_PRIVATE_PATH = re.compile(
    rb"(?:" + _LINUX_HOME_ROOT + rb"|" + _MACOS_HOME_ROOT + rb"|" + _WINDOWS_HOME_ROOT
    + rb")(?!USER/|<user>/|USER\\|<user>\\)"
)


class HostArtifactError(ValueError):
    """The artifact directory does not satisfy the canonical contract."""


@dataclass(frozen=True)
class HostWheelArtifact:
    wheel: Path
    checksum: Path
    digest: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_distribution_version(value: str) -> str:
    if _VERSION.fullmatch(value) is None:
        raise HostArtifactError("distribution version must be strict X.Y.Z")
    return value


def read_distribution_version(source_root: Path) -> str:
    try:
        data = tomllib.loads((source_root / "host" / "pyproject.toml").read_text(encoding="utf-8"))
        version = data["project"]["version"]
    except (FileNotFoundError, KeyError, TypeError, tomllib.TOMLDecodeError) as exc:
        raise HostArtifactError("could not read host distribution version") from exc
    if not isinstance(version, str):
        raise HostArtifactError("host distribution version must be a string")
    return validate_distribution_version(version)


def wheel_basename(distribution_version: str) -> str:
    return f"s3_hidbot_host-{validate_distribution_version(distribution_version)}-py3-none-any.whl"


def checksum_basename(distribution_version: str) -> str:
    return f"{wheel_basename(distribution_version)}.sha256"


def dist_info_name(distribution_version: str) -> str:
    return f"s3_hidbot_host-{validate_distribution_version(distribution_version)}.dist-info"


def checksum_text(digest: str, wheel_name: str, distribution_version: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise HostArtifactError("checksum digest must be 64 lowercase hexadecimal characters")
    if wheel_name != wheel_basename(distribution_version):
        raise HostArtifactError("checksum wheel basename is not canonical")
    return f"{digest}  {wheel_name}\n"


def _metadata_value(metadata: str, field: str) -> str:
    prefix = f"{field}: "
    for line in metadata.splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :]
    raise HostArtifactError(f"wheel metadata is missing {field}")


def validate_wheel(wheel: Path, distribution_version: str) -> None:
    expected_basename = wheel_basename(distribution_version)
    dist_info = dist_info_name(distribution_version)
    if wheel.name != expected_basename:
        raise HostArtifactError(f"unexpected wheel filename: {wheel.name}")
    try:
        with zipfile.ZipFile(wheel) as archive:
            if archive.testzip() is not None:
                raise HostArtifactError("wheel ZIP CRC validation failed")
            names = archive.namelist()
            if any(name.endswith("/") for name in names):
                raise HostArtifactError("wheel must not contain directory members")
            for name in names:
                path = PurePosixPath(name)
                if path.is_absolute() or ".." in path.parts:
                    raise HostArtifactError(f"unsafe wheel member path: {name}")
                if not (name.startswith("hidbot/") or name.startswith(f"{dist_info}/")):
                    raise HostArtifactError(f"unexpected wheel payload path: {name}")
                if (
                    name.startswith("tests/")
                    or "/tests/" in name
                    or "__pycache__" in name
                    or name.endswith(".pyc")
                    or name == ".envrc"
                    or name.endswith("/.envrc")
                    or name.startswith("tools/")
                    or name.startswith("firmware/")
                ):
                    raise HostArtifactError(f"forbidden wheel payload path: {name}")
            payload = {name: archive.read(name) for name in names}
    except zipfile.BadZipFile as error:
        raise HostArtifactError("wheel is not a valid ZIP archive") from error

    package_modules = {name for name in payload if name.startswith("hidbot/")}
    if package_modules != REQUIRED_MODULES:
        missing = sorted(REQUIRED_MODULES.difference(package_modules))
        unexpected = sorted(package_modules.difference(REQUIRED_MODULES))
        raise HostArtifactError(
            f"wheel runtime module set does not match; missing={missing}, unexpected={unexpected}"
        )
    for name, content in payload.items():
        if _PRIVATE_PATH.search(content):
            raise HostArtifactError(f"developer-specific path in wheel payload: {name}")
        if b"/dev/serial/by-id/" in content:
            raise HostArtifactError(f"serial by-id path in wheel payload: {name}")

    metadata = payload.get(f"{dist_info}/METADATA")
    wheel_metadata = payload.get(f"{dist_info}/WHEEL")
    entry_points = payload.get(f"{dist_info}/entry_points.txt")
    if metadata is None or wheel_metadata is None or entry_points is None:
        raise HostArtifactError("wheel is missing required dist-info metadata")
    decoded_metadata = metadata.decode("utf-8")
    if _metadata_value(decoded_metadata, "Metadata-Version") != "2.4":
        raise HostArtifactError("wheel core metadata version does not match")
    if _metadata_value(decoded_metadata, "Name") != DISTRIBUTION_NAME:
        raise HostArtifactError("wheel distribution metadata does not match")
    if _metadata_value(decoded_metadata, "Version") != distribution_version:
        raise HostArtifactError("wheel version metadata does not match")
    if _metadata_value(decoded_metadata, "Requires-Python") != ">=3.11":
        raise HostArtifactError("wheel Requires-Python metadata does not match")
    if "Requires-Dist: pyserial<4,>=3.5" not in decoded_metadata:
        raise HostArtifactError("wheel runtime dependency metadata does not match")
    if "Provides-Extra: flash" not in decoded_metadata:
        raise HostArtifactError("wheel flash extra metadata is missing")
    if "Requires-Dist: esptool<5,>=4.12; extra == \"flash\"" not in decoded_metadata:
        raise HostArtifactError("wheel flash dependency metadata does not match")
    if _metadata_value(decoded_metadata, "License-Expression") != "MIT":
        raise HostArtifactError("wheel SPDX license expression does not match")
    if _metadata_value(decoded_metadata, "License-File") != "LICENSE":
        raise HostArtifactError("wheel license file metadata does not match")
    license_text = payload.get(f"{dist_info}/licenses/LICENSE")
    if license_text is None or b"MIT License" not in license_text:
        raise HostArtifactError("wheel MIT license file is missing")
    decoded_wheel = wheel_metadata.decode("utf-8")
    if "Root-Is-Purelib: true" not in decoded_wheel or "Tag: py3-none-any" not in decoded_wheel:
        raise HostArtifactError("wheel tag is not py3-none-any pure Python")
    if "hidbotctl = hidbot.cli:main" not in entry_points.decode("utf-8"):
        raise HostArtifactError("wheel console entry point does not match")


def validate_artifact_directory(directory: Path, distribution_version: str) -> HostWheelArtifact:
    if not directory.is_dir():
        raise HostArtifactError(f"artifact directory does not exist: {directory}")
    entries = sorted(directory.iterdir())
    expected_wheel = wheel_basename(distribution_version)
    expected_checksum = checksum_basename(distribution_version)
    expected = {expected_wheel, expected_checksum}
    actual = {path.name for path in entries}
    if actual != expected or len(entries) != 2 or not all(path.is_file() for path in entries):
        raise HostArtifactError(f"artifact directory must contain only wheel and checksum: {sorted(actual)}")
    wheel = directory / expected_wheel
    checksum = directory / expected_checksum
    try:
        text = checksum.read_text(encoding="ascii")
    except UnicodeDecodeError as error:
        raise HostArtifactError("checksum file is not ASCII") from error
    match = _CHECKSUM.fullmatch(text)
    if match is None:
        raise HostArtifactError("checksum file syntax is not canonical")
    if match.group(1) != wheel.name:
        raise HostArtifactError("checksum filename does not match wheel")
    digest = sha256_file(wheel)
    if text[:64] != digest:
        raise HostArtifactError("checksum digest does not match wheel")
    validate_wheel(wheel, distribution_version)
    return HostWheelArtifact(wheel=wheel, checksum=checksum, digest=digest)
