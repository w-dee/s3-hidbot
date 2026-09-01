"""Prepare and verify the exact temporary asset set for a public release."""

from __future__ import annotations

import shutil
import tarfile
from pathlib import Path, PurePosixPath

from firmware_artifact import ArtifactError, sha256_file, verify_bundle_archive
from host_artifact import HostArtifactError, validate_wheel
from host_build import build_host_distributions
from release_contract import ReleaseContract, ReleaseContractError, validate_source_revision


class ReleaseAssetError(ValueError):
    """The release asset directory is incomplete or internally inconsistent."""


def checksum_text(digest: str, basename: str) -> str:
    if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
        raise ReleaseAssetError("checksum digest must be lowercase SHA-256")
    return f"{digest}  {basename}\n"


def _copy_regular(source: Path, destination: Path) -> None:
    if source.is_symlink() or not source.is_file():
        raise ReleaseAssetError(f"release input is not a regular file: {source.name}")
    shutil.copyfile(source, destination)


def _write_checksum(directory: Path, basename: str) -> None:
    primary = directory / basename
    (directory / f"{basename}.sha256").write_text(
        checksum_text(sha256_file(primary), basename),
        encoding="ascii",
    )


def _validate_checksum(directory: Path, basename: str) -> None:
    checksum = directory / f"{basename}.sha256"
    try:
        actual = checksum.read_text(encoding="ascii")
    except (FileNotFoundError, UnicodeDecodeError) as exc:
        raise ReleaseAssetError(f"checksum is missing or not ASCII for {basename}") from exc
    expected = checksum_text(sha256_file(directory / basename), basename)
    if actual != expected:
        raise ReleaseAssetError(f"checksum does not match {basename}")


def _validate_sdist(path: Path, contract: ReleaseContract) -> None:
    expected_root = f"s3_hidbot_host-{contract.version}"
    try:
        with tarfile.open(path, "r:gz") as archive:
            members = archive.getmembers()
            roots = {PurePosixPath(member.name).parts[0] for member in members if member.name}
            if roots != {expected_root}:
                raise ReleaseAssetError("sdist must contain exactly its canonical top-level directory")
            metadata = archive.extractfile(f"{expected_root}/PKG-INFO")
            if metadata is None:
                raise ReleaseAssetError("sdist is missing PKG-INFO")
            text = metadata.read().decode("utf-8")
    except (tarfile.TarError, UnicodeDecodeError) as exc:
        raise ReleaseAssetError("host sdist is not a valid UTF-8 gzip tar archive") from exc
    if "Name: s3-hidbot-host\n" not in text or f"Version: {contract.version}\n" not in text:
        raise ReleaseAssetError("host sdist metadata does not match the release contract")


def validate_release_asset_directory(
    directory: Path,
    contract: ReleaseContract,
    *,
    source_revision: str | None = None,
) -> dict:
    """Verify names, sidecars, firmware contract, legal assets, and host metadata."""

    if not directory.is_dir():
        raise ReleaseAssetError("release asset directory does not exist")
    entries = sorted(directory.iterdir(), key=lambda path: path.name)
    actual = tuple(path.name for path in entries)
    expected = tuple(sorted(contract.release_assets))
    if actual != expected or not all(path.is_file() and not path.is_symlink() for path in entries):
        raise ReleaseAssetError(f"release asset set is not exact: expected {list(expected)}, got {list(actual)}")
    for primary in contract.distributable_assets[::2] + contract.legal_assets[::2]:
        _validate_checksum(directory, primary)
    firmware = directory / contract.firmware_archive
    try:
        manifest = verify_bundle_archive(firmware)
    except ArtifactError as exc:
        raise ReleaseAssetError("firmware archive failed official verification") from exc
    firmware_data = manifest["firmware"]
    if firmware_data["version"] != contract.version:
        raise ReleaseAssetError("firmware archive version does not match release contract")
    revision = validate_source_revision(firmware_data["source_revision"])
    if source_revision is not None and revision != validate_source_revision(source_revision):
        raise ReleaseAssetError("firmware archive source revision does not match expected commit")
    try:
        validate_wheel(directory / contract.host_wheel, contract.version)
    except HostArtifactError as exc:
        raise ReleaseAssetError("host wheel failed canonical validation") from exc
    _validate_sdist(directory / contract.host_sdist, contract)
    return manifest


def prepare_release_assets(
    output_directory: Path,
    source_root: Path,
    firmware_archive: Path,
    contract: ReleaseContract,
    *,
    source_revision: str | None = None,
) -> dict:
    """Assemble release assets from one already-verified firmware archive."""

    output = output_directory.resolve()
    if output.exists() and any(output.iterdir()):
        raise ReleaseAssetError("release asset output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    source = firmware_archive.resolve()
    if source.name != contract.firmware_archive:
        raise ReleaseAssetError("firmware archive filename does not match release contract")
    _copy_regular(source, output / source.name)
    _write_checksum(output, source.name)
    distributions = build_host_distributions(
        source_root,
        output,
        wheel=True,
        sdist=True,
    )
    distribution_names = {path.name for path in distributions}
    if distribution_names != {contract.host_wheel, contract.host_sdist}:
        raise ReleaseAssetError("host build output filenames do not match release contract")
    for primary in (contract.host_wheel, contract.host_sdist):
        _write_checksum(output, primary)
    for legal in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        _copy_regular(source_root / legal, output / legal)
        _write_checksum(output, legal)
    return validate_release_asset_directory(output, contract, source_revision=source_revision)
