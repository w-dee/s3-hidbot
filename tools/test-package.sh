#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

find_repository_artifact() {
    git -C "$repository_root" ls-files --cached --others --exclude-standard -- host \
        | awk '/(^|\/)(build|dist|[^/]+\.egg-info)(\/|$)/ { print; exit }'
}

if repository_artifact=$(find_repository_artifact) && [[ -n "$repository_artifact" ]]; then
    echo "repository packaging artifact exists before validation: $repository_artifact" >&2
    exit 1
fi

"$python_bin" -m venv "$temporary_directory/tools-venv"
tools_python="$temporary_directory/tools-venv/bin/python"
"$tools_python" -m pip install --disable-pip-version-check --no-input build twine

for build_number in 1 2; do
    stage_host="$temporary_directory/stage-$build_number/host"
    mkdir -p "$stage_host/src"
    cp "$repository_root/host/LICENSE" "$repository_root/host/MANIFEST.in" \
        "$repository_root/host/README.md" "$repository_root/host/pyproject.toml" \
        "$stage_host/"
    cp -R "$repository_root/host/src/hidbot" "$stage_host/src/hidbot"
    cp -R "$repository_root/host/tests" "$stage_host/tests"
    "$tools_python" -m build \
        --outdir "$temporary_directory/dist-$build_number" \
        "$stage_host"
done

wheel_one=$(find "$temporary_directory/dist-1" -maxdepth 1 -type f -name '*.whl' -print -quit)
sdist_one=$(find "$temporary_directory/dist-1" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
wheel_two=$(find "$temporary_directory/dist-2" -maxdepth 1 -type f -name '*.whl' -print -quit)
sdist_two=$(find "$temporary_directory/dist-2" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
for artifact in "$wheel_one" "$sdist_one" "$wheel_two" "$sdist_two"; do
    [[ -n "$artifact" && -f "$artifact" ]] || {
        echo "expected wheel and sdist artifacts were not built" >&2
        exit 1
    }
done

twine_log="$temporary_directory/twine-check.log"
if ! "$tools_python" -m twine check "$wheel_one" "$sdist_one" >"$twine_log" 2>&1; then
    cat "$twine_log" >&2
    exit 1
fi
if grep -q 'WARNING\|ERROR' "$twine_log"; then
    cat "$twine_log" >&2
    exit 1
fi
cat "$twine_log"

"$tools_python" - "$wheel_one" "$sdist_one" "$wheel_two" "$sdist_two" \
    "$repository_root/LICENSE" "$repository_root/host/LICENSE" <<'PY'
from __future__ import annotations

import re
import sys
import tarfile
import zipfile
from pathlib import PurePosixPath

wheel_one, sdist_one, wheel_two, sdist_two, root_license, package_license = sys.argv[1:]


def wheel_contents(path: str) -> dict[str, bytes]:
    with zipfile.ZipFile(path) as archive:
        return {name: archive.read(name) for name in archive.namelist()}


def sdist_contents(path: str) -> tuple[str, dict[str, bytes]]:
    with tarfile.open(path, "r:gz") as archive:
        names = archive.getnames()
        roots = {PurePosixPath(name).parts[0] for name in names if name}
        assert len(roots) == 1, f"sdist must have one root directory: {roots}"
        root = next(iter(roots))
        contents: dict[str, bytes] = {}
        for name in names:
            relative = name[len(root) + 1 :] if name.startswith(root + "/") else name
            member = archive.getmember(name)
            if member.isfile():
                extracted = archive.extractfile(member)
                assert extracted is not None
                contents[relative] = extracted.read()
        return root, contents


wheel = wheel_contents(wheel_one)
_, sdist = sdist_contents(sdist_one)
wheel_repeat = wheel_contents(wheel_two)
_, sdist_repeat = sdist_contents(sdist_two)

assert set(wheel) == set(wheel_repeat), "wheel member sets differ between builds"
assert all(wheel[name] == wheel_repeat[name] for name in wheel), "wheel payloads differ"
assert set(sdist) == set(sdist_repeat), "sdist member sets differ between builds"
assert all(sdist[name] == sdist_repeat[name] for name in sdist), "sdist payloads differ"

package_files = {
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
    "hidbot/serial_transport.py",
}
assert package_files.issubset(wheel), "wheel is missing a host package module"
assert all(
    name.startswith("hidbot/") or ".dist-info/" in name for name in wheel
), "wheel contains a path outside package or dist-info"
assert not any("/tests/" in name or name.startswith("tests/") for name in wheel)
assert not any("__pycache__" in name or name.endswith(".pyc") for name in wheel)
assert not any(name.endswith("/.envrc") or name == ".envrc" for name in wheel)
assert not any("/build/" in name or "/dist/" in name for name in wheel)
metadata_name = next(name for name in wheel if name.endswith(".dist-info/METADATA"))
metadata = wheel[metadata_name].decode("utf-8")
assert "Name: s3-hidbot-host\n" in metadata
assert "Version: 0.1.0\n" in metadata
assert "Requires-Python: >=3.11\n" in metadata
assert "Requires-Dist: pyserial<4,>=3.5\n" in metadata
assert "Provides-Extra: flash\n" in metadata
assert 'Requires-Dist: esptool<5,>=4.12; extra == "flash"\n' in metadata
assert any(line.startswith("License: MIT") for line in metadata.splitlines())
assert "Description-Content-Type: text/markdown\n" in metadata
entry_points_name = next(name for name in wheel if name.endswith(".dist-info/entry_points.txt"))
assert b"hidbotctl = hidbot.cli:main" in wheel[entry_points_name]
license_members = [
    name
    for name in wheel
    if name.endswith(".dist-info/licenses/LICENSE") or name.endswith(".dist-info/LICENSE")
]
assert license_members, "wheel does not contain its package license file"

root_license_bytes = open(root_license, "rb").read()
package_license_bytes = open(package_license, "rb").read()
assert root_license_bytes == package_license_bytes, "root and package license drift"
assert b"MIT License" in root_license_bytes
assert b"Copyright (c) 2026 W.Dee" in root_license_bytes
assert wheel[license_members[0]] == package_license_bytes

allowed_sdist_files = {
    "LICENSE",
    "MANIFEST.in",
    "PKG-INFO",
    "README.md",
    "pyproject.toml",
    "setup.cfg",
}
expected_tests = {
    "tests/__init__.py",
    "tests/test_cli.py",
    "tests/test_client.py",
    "tests/test_firmware_verification.py",
    "tests/test_flashing.py",
    "tests/test_framing.py",
    "tests/test_protocol.py",
    "tests/test_provisioning.py",
    "tests/test_serial_transport.py",
}
assert expected_tests.issubset(sdist), "sdist is missing a complete test suite"
for name in sdist:
    if name.startswith("src/hidbot/"):
        assert name.endswith(".py"), f"unexpected host package artifact: {name}"
    elif name.startswith("src/s3_hidbot_host.egg-info/"):
        pass
    elif name.startswith("tests/"):
        assert name in expected_tests, f"unexpected sdist test file: {name}"
    else:
        assert name in allowed_sdist_files, f"unexpected sdist file: {name}"
assert sdist["LICENSE"] == package_license_bytes
assert b"s3-hidbot-host" in sdist["README.md"]
assert not any(".envrc" in name or "__pycache__" in name for name in sdist)
assert not any("/build/" in name or "/dist/" in name for name in sdist)

linux_root = b"/" + b"home" + b"/"
macos_root = b"/" + b"Users" + b"/"
privacy_patterns = (
    re.compile(linux_root + rb"(?!USER/|<user>/)[^/\r\n]+/"),
    re.compile(macos_root + rb"(?!USER/|<user>/)[^/\r\n]+/"),
    re.compile(rb"[A-Za-z]:\\Users\\(?!USER\\|<user>\\)[^\\\r\n]+\\"),
)
for archive_name, files in (("wheel", wheel), ("sdist", sdist)):
    for name, payload in files.items():
        for pattern in privacy_patterns:
            assert not pattern.search(payload), f"privacy path in {archive_name}: {name}"
        assert b"/dev/serial/by-id/" not in payload
print("PASS: package artifacts, metadata, privacy, and semantic reproducibility")
PY

install_and_smoke() {
    local artifact=$1
    local venv_directory=$2
    local venv_python="$venv_directory/bin/python"
    local venv_cli="$venv_directory/bin/hidbotctl"
    "$python_bin" -m venv "$venv_directory"
    "$venv_python" -m pip install --disable-pip-version-check --no-input "$artifact"
    "$venv_python" -m pip check
    env -u PYTHONPATH "$venv_python" - "$artifact" <<'PY'
from __future__ import annotations

import sys
from importlib.metadata import version
from pathlib import Path

artifact = sys.argv[1]
import hidbot
from hidbot.artifact import verify_bundle_archive
from hidbot.firmware_verification import compare_firmware_identity

assert version("s3-hidbot-host") == "0.1.0"
assert not hasattr(hidbot, "__version__")
assert "site-packages" in Path(hidbot.__file__).parts
assert callable(verify_bundle_archive)
assert callable(compare_firmware_identity)
print(f"PASS: installed import/version check for {Path(artifact).name}")
PY
    env -u PYTHONPATH "$venv_cli" --help >/dev/null
    env -u PYTHONPATH "$venv_cli" flash-firmware --help >/dev/null
    for command in hello ping info usb-status release-all self-test; do
        set +e
        env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD \
            "$venv_cli" "$command" >"$temporary_directory/cli-$command.log" 2>&1
        status=$?
        set -e
        if [[ "$status" -ne 2 ]]; then
            cat "$temporary_directory/cli-$command.log" >&2
            echo "unexpected CLI smoke status for $command: $status" >&2
            exit 1
        fi
    done
    set +e
    env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD \
        "$venv_cli" verify-firmware "$temporary_directory/missing-artifact" \
        >"$temporary_directory/cli-verify-firmware.log" 2>&1
    status=$?
    set -e
    if [[ "$status" -ne 2 ]]; then
        cat "$temporary_directory/cli-verify-firmware.log" >&2
        echo "unexpected CLI smoke status for verify-firmware: $status" >&2
        exit 1
    fi
    set +e
    env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD \
        "$venv_cli" verify-artifact "$temporary_directory/missing-artifact" \
        >"$temporary_directory/cli-verify-artifact.log" 2>&1
    status=$?
    set -e
    if [[ "$status" -ne 2 ]] || ! grep -q '^artifact error:' "$temporary_directory/cli-verify-artifact.log"; then
        cat "$temporary_directory/cli-verify-artifact.log" >&2
        echo "verify-artifact installed-wheel smoke did not reach artifact validation" >&2
        exit 1
    fi
    for primitive in keyboard-report mouse-report; do
        local -a primitive_args
        if [[ "$primitive" == "keyboard-report" ]]; then
            primitive_args=(keyboard-report --unsafe-hid --modifiers 0 --key 0x04)
        else
            primitive_args=(mouse-report --unsafe-hid --buttons 0 --x 1 --y 0 --wheel 0 --pan 0)
        fi
        set +e
        env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD \
            "$venv_cli" "${primitive_args[@]}" >"$temporary_directory/cli-$primitive.log" 2>&1
        status=$?
        set -e
        if [[ "$status" -ne 2 ]]; then
            cat "$temporary_directory/cli-$primitive.log" >&2
            echo "unexpected CLI smoke status for $primitive: $status" >&2
            exit 1
        fi
    done
    flash_bundle_parent="$temporary_directory/flash-missing-extra-bundle"
    flash_bundle=$(env -u PYTHONPATH "$venv_python" - "$repository_root" "$flash_bundle_parent" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "host" / "tests"))
from test_provisioning import _make_bundle

print(_make_bundle(Path(sys.argv[2])))
PY
    )
    set +e
    env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD \
        "$venv_cli" --port missing-port flash-firmware "$flash_bundle" \
        >"$temporary_directory/cli-flash-firmware.log" 2>&1
    status=$?
    set -e
    if [[ "$status" -ne 2 ]] || ! grep -q 's3-hidbot-host\[flash\]' "$temporary_directory/cli-flash-firmware.log"; then
        cat "$temporary_directory/cli-flash-firmware.log" >&2
        echo "base wheel flash-firmware missing-extra smoke failed" >&2
        exit 1
    fi
    env -u PYTHONPATH "$venv_python" -c 'import importlib.util; assert importlib.util.find_spec("esptool") is None'
}

install_flash_extra_and_smoke() {
    local artifact=$1
    local venv_directory=$2
    local venv_python="$venv_directory/bin/python"
    local venv_cli="$venv_directory/bin/hidbotctl"
    "$python_bin" -m venv "$venv_directory"
    "$venv_python" -m pip install --disable-pip-version-check --no-input "${artifact}[flash]"
    "$venv_python" -m pip check
    env -u PYTHONPATH "$venv_python" -m esptool version >/dev/null
    env -u PYTHONPATH "$venv_cli" flash-firmware --help >/dev/null
    env -u PYTHONPATH "$venv_python" - <<'PY'
from importlib.metadata import version

value = version("esptool")
parts = tuple(int(part) for part in value.split(".")[:2])
assert parts >= (4, 12) and parts < (5, 0), value
print(f"PASS: installed flash extra esptool {value}")
PY
}

install_and_smoke "$wheel_one" "$temporary_directory/wheel-venv"
install_flash_extra_and_smoke "$wheel_one" "$temporary_directory/flash-wheel-venv"

sdist_extract_directory="$temporary_directory/sdist-extracted"
mkdir -p "$sdist_extract_directory"
tar -xzf "$sdist_one" -C "$sdist_extract_directory"
sdist_source_directory=$(find "$sdist_extract_directory" -mindepth 1 -maxdepth 1 -type d -print -quit)
[[ -n "$sdist_source_directory" ]] || {
    echo "could not locate extracted sdist source" >&2
    exit 1
}
install_and_smoke "$sdist_one" "$temporary_directory/sdist-venv"
sdist_python="$temporary_directory/sdist-venv/bin/python"
(
    cd "$sdist_source_directory"
    env -u PYTHONPATH "$sdist_python" -m unittest discover -s tests
)

if repository_artifact=$(find_repository_artifact) && [[ -n "$repository_artifact" ]]; then
    echo "repository packaging artifact leaked during validation: $repository_artifact" >&2
    exit 1
fi

echo "PASS: host package validation (wheel, sdist, clean installs, extracted-sdist tests)"
