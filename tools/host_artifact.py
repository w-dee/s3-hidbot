"""Validation for the U6.4A canonical downloadable host wheel."""

from __future__ import annotations

import hashlib
import re
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


DISTRIBUTION_NAME = "s3-hidbot-host"
DISTRIBUTION_VERSION = "0.1.0"
WHEEL_BASENAME = "s3_hidbot_host-0.1.0-py3-none-any.whl"
CHECKSUM_BASENAME = f"{WHEEL_BASENAME}.sha256"
DIST_INFO = "s3_hidbot_host-0.1.0.dist-info"
REQUIRED_MODULES = frozenset(
    {
        "hidbot/__init__.py",
        "hidbot/artifact.py",
        "hidbot/cli.py",
        "hidbot/client.py",
        "hidbot/errors.py",
        "hidbot/firmware_verification.py",
        "hidbot/framing.py",
        "hidbot/protocol.py",
        "hidbot/provisioning.py",
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


def checksum_text(digest: str, wheel_basename: str) -> str:
    if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise HostArtifactError("checksum digest must be 64 lowercase hexadecimal characters")
    if wheel_basename != WHEEL_BASENAME:
        raise HostArtifactError("checksum wheel basename is not canonical")
    return f"{digest}  {wheel_basename}\n"


def _metadata_value(metadata: str, field: str) -> str:
    prefix = f"{field}: "
    for line in metadata.splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :]
    raise HostArtifactError(f"wheel metadata is missing {field}")


def _validate_wheel(wheel: Path) -> None:
    if wheel.name != WHEEL_BASENAME:
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
                if not (name.startswith("hidbot/") or name.startswith(f"{DIST_INFO}/")):
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

    metadata = payload.get(f"{DIST_INFO}/METADATA")
    wheel_metadata = payload.get(f"{DIST_INFO}/WHEEL")
    entry_points = payload.get(f"{DIST_INFO}/entry_points.txt")
    if metadata is None or wheel_metadata is None or entry_points is None:
        raise HostArtifactError("wheel is missing required dist-info metadata")
    decoded_metadata = metadata.decode("utf-8")
    if _metadata_value(decoded_metadata, "Name") != DISTRIBUTION_NAME:
        raise HostArtifactError("wheel distribution metadata does not match")
    if _metadata_value(decoded_metadata, "Version") != DISTRIBUTION_VERSION:
        raise HostArtifactError("wheel version metadata does not match")
    if _metadata_value(decoded_metadata, "Requires-Python") != ">=3.11":
        raise HostArtifactError("wheel Requires-Python metadata does not match")
    if "Requires-Dist: pyserial<4,>=3.5" not in decoded_metadata:
        raise HostArtifactError("wheel runtime dependency metadata does not match")
    decoded_wheel = wheel_metadata.decode("utf-8")
    if "Root-Is-Purelib: true" not in decoded_wheel or "Tag: py3-none-any" not in decoded_wheel:
        raise HostArtifactError("wheel tag is not py3-none-any pure Python")
    if "hidbotctl = hidbot.cli:main" not in entry_points.decode("utf-8"):
        raise HostArtifactError("wheel console entry point does not match")


def validate_artifact_directory(directory: Path) -> HostWheelArtifact:
    if not directory.is_dir():
        raise HostArtifactError(f"artifact directory does not exist: {directory}")
    entries = sorted(directory.iterdir())
    expected = {WHEEL_BASENAME, CHECKSUM_BASENAME}
    actual = {path.name for path in entries}
    if actual != expected or len(entries) != 2 or not all(path.is_file() for path in entries):
        raise HostArtifactError(f"artifact directory must contain only wheel and checksum: {sorted(actual)}")
    wheel = directory / WHEEL_BASENAME
    checksum = directory / CHECKSUM_BASENAME
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
    _validate_wheel(wheel)
    return HostWheelArtifact(wheel=wheel, checksum=checksum, digest=digest)
