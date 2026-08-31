#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

before=$(git -C "$repository_root" status --porcelain --ignored -- host/build host/dist host/src)
"$python_bin" -m venv "$temporary_directory/tools-venv"
tools_python="$temporary_directory/tools-venv/bin/python"
PIP_CACHE_DIR="$temporary_directory/pip-cache" \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    "$tools_python" -m pip install --no-input build

source_root="$temporary_directory/source"
mkdir -p "$source_root"
tar -C "$repository_root" \
    --exclude='__pycache__' --exclude='*.pyc' --exclude='*.pyo' \
    -cf - host/LICENSE host/MANIFEST.in host/README.md host/pyproject.toml host/src/hidbot host/tests \
    | tar -C "$source_root" -xf -
artifact_directory="$temporary_directory/artifact"
"$tools_python" "$repository_root/tools/build_host_artifact.py" \
    --source-root "$source_root" "$artifact_directory"
"$tools_python" "$repository_root/tools/test_host_artifact.py" "$artifact_directory"
artifact_sha256=$(sha256sum "$artifact_directory/s3_hidbot_host-0.1.0-py3-none-any.whl" | awk '{print $1}')

"$python_bin" -m venv "$temporary_directory/consumer-venv"
consumer_python="$temporary_directory/consumer-venv/bin/python"
consumer_cli="$temporary_directory/consumer-venv/bin/hidbotctl"
PIP_CACHE_DIR="$temporary_directory/pip-cache" \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    "$consumer_python" -m pip install --no-input \
    "$artifact_directory/s3_hidbot_host-0.1.0-py3-none-any.whl"
"$consumer_python" -m pip check
env -u PYTHONPATH "$consumer_python" -c '
from importlib.metadata import version
from pathlib import Path
import hidbot

assert version("s3-hidbot-host") == "0.1.0"
assert "site-packages" in Path(hidbot.__file__).parts
'
env -u PYTHONPATH "$consumer_cli" --help >/dev/null
set +e
env -u PYTHONPATH -u S3_HIDBOT_SERIAL -u S3_HIDBOT_BAUD "$consumer_cli" hello >/dev/null 2>&1
consumer_status=$?
set -e
if [[ "$consumer_status" -ne 2 ]]; then
    echo "unexpected no-serial consumer CLI status: $consumer_status" >&2
    exit 1
fi

mapfile -t artifacts < <(find "$artifact_directory" -maxdepth 1 -type f -printf '%f\n' | sort)
expected=(s3_hidbot_host-0.1.0-py3-none-any.whl s3_hidbot_host-0.1.0-py3-none-any.whl.sha256)
if [[ "${artifacts[*]}" != "${expected[*]}" ]]; then
    echo "unexpected canonical host artifact contents: ${artifacts[*]}" >&2
    exit 1
fi
after=$(git -C "$repository_root" status --porcelain --ignored -- host/build host/dist host/src)
if [[ "$before" != "$after" ]]; then
    echo "host packaging residue changed during artifact producer test" >&2
    exit 1
fi

echo "PASS: canonical host artifact producer isolation ($artifact_sha256)"
