#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}

PYTHONPATH="$repository_root/tools:$repository_root/host/src${PYTHONPATH:+:$PYTHONPATH}" \
  "$python_bin" "$repository_root/tools/test_qualification_harness.py"

echo "PASS: hardware-free qualification harness tests"
