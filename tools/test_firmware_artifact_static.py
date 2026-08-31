#!/usr/bin/env python3
"""Static guards for the source-independent U6.3A artifact boundary."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / "tools/build_firmware_artifact.py"
VERIFIER = ROOT / "tools/verify_firmware_artifact.py"
LIBRARY = ROOT / "tools/firmware_artifact.py"
CANONICAL_LIBRARY = ROOT / "host/src/hidbot/artifact.py"
FOCUSED = ROOT / "tools/test-firmware-artifact.sh"
DEFAULTS = ROOT / "firmware/sdkconfig.artifact.defaults"


def main() -> int:
    builder = BUILDER.read_text(encoding="utf-8")
    verifier = VERIFIER.read_text(encoding="utf-8")
    adapter = LIBRARY.read_text(encoding="utf-8")
    library = CANONICAL_LIBRARY.read_text(encoding="utf-8")
    focused = FOCUSED.read_text(encoding="utf-8")
    defaults = DEFAULTS.read_text(encoding="utf-8")

    assert "CONFIG_APP_REPRODUCIBLE_BUILD=y" in defaults
    assert "TemporaryDirectory" in builder
    assert "SDKCONFIG_DEFAULTS" in builder
    assert "S3_HIDBOT_SOURCE_REVISION" in builder
    assert "SOURCE_DATE_EPOCH" in builder
    assert "verify_bundle_directory" in builder
    assert "verify_bundle_archive" in builder
    assert "create_deterministic_tar_gz" in builder
    assert "git rev-parse" not in builder
    assert "git describe" not in builder
    assert ".git" not in builder
    assert "firmware/build" not in builder
    assert not re.search(r"\[\s*[\"']git[\"']", builder)
    assert "--hardware" not in builder
    assert "/dev/input" not in builder
    assert "serial" not in builder.replace("S3_HIDBOT_SERIAL", "").lower()
    assert "Client" not in builder
    assert "flash helper" not in builder.lower()

    assert "verify_bundle_archive" in verifier
    assert "subprocess" not in verifier
    assert "serial" not in verifier.lower()
    assert "/dev/input" not in verifier
    assert "Client" not in verifier

    assert CANONICAL_LIBRARY.is_file()
    assert "spec_from_file_location" in adapter
    assert "host" in adapter and "artifact.py" in adapter
    assert "import hidbot" not in adapter
    assert "def verify_bundle_directory" not in adapter
    assert "def verify_bundle_archive" not in adapter
    assert "def validate_hash" not in adapter

    assert "SHA256SUMS" in library
    assert "manifest.json" in library
    assert "source_date_epoch" in library
    assert "symlink" in library
    assert ".." in library
    assert "@sha256:" in library
    assert "--hardware" not in library

    assert "test_firmware_artifact.py" in focused
    assert "build_firmware_artifact.py" in focused
    assert "source-revision" in focused
    assert "source-date-epoch" in focused
    assert "cmp" in focused
    assert "firmware/build" not in focused
    print("PASS: firmware artifact build and verifier static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
