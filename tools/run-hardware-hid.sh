#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}
temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/s3-hidbot-hardware.XXXXXX")

cleanup() {
  rm -rf -- "$temporary_root" || true
}
trap cleanup EXIT

venv_path="$temporary_root/venv"
pip_cache_path="$temporary_root/pip-cache"

if ! "$python_bin" -m venv "$venv_path"; then
  echo "ERROR: could not create temporary hardware-runner virtualenv" >&2
  exit 70
fi

if ! TMPDIR="$temporary_root" PIP_CACHE_DIR="$pip_cache_path" PIP_DISABLE_PIP_VERSION_CHECK=1 \
  "$venv_path/bin/python" -m pip install "$repository_root/host"; then
  echo "ERROR: could not install the host package in the temporary virtualenv; dependency retrieval may require network access" >&2
  exit 71
fi

set +e
"$venv_path/bin/python" "$repository_root/tools/hid_hardware_smoke.py" "$@"
runner_status=$?
set -e

exit "$runner_status"
