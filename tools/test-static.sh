#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}

"$python_bin" "$repository_root/tools/test_control_protocol_static.py"
"$python_bin" "$repository_root/tools/test_default_hid_safety.py"
"$python_bin" "$repository_root/tools/test_hid_runtime_static.py"
"$python_bin" "$repository_root/tools/test_uart_control_transport_static.py"
"$python_bin" "$repository_root/tools/test_documentation.py"

echo "PASS: static validation suite"
