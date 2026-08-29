#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}

if [[ -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1; then
    echo "all non-hardware validation requires an active ESP-IDF v5.5.4 environment" >&2
    exit 2
fi

"$repository_root/tools/test-static.sh"
"$python_bin" "$repository_root/tools/test_privacy_lint.py"
"$python_bin" "$repository_root/tools/privacy_lint.py" --tracked
"$repository_root/tools/test-host.sh"
"$repository_root/tools/test-native.sh"
"$repository_root/tools/test-control-protocol.sh"
git -C "$repository_root" diff --check

echo "PASS: all non-hardware validation suites"
