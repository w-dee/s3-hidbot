#!/usr/bin/env bash
set -euo pipefail
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHONDONTWRITEBYTECODE=1 "${PYTHON_BIN:-python3}" -m unittest discover -s "$repository_root/tools/rp-test" -p 'test_*.py'
