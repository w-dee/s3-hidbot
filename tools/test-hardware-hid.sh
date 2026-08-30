#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}

for argument in "$@"; do
  if [[ "$argument" == "--hardware" ]]; then
    exec "$python_bin" "$repository_root/tools/hid_hardware_smoke.py" "$@"
  fi
done

if (( $# > 0 )); then
  echo "ERROR: pass --hardware to run the physical observer; no-hardware mode takes no arguments" >&2
  exit 2
fi

"$python_bin" "$repository_root/tools/test_hid_hardware_smoke.py"
echo "PASS: read-only HID observer/discovery and F24 smoke unit tests"
