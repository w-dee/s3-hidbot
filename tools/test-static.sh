#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}

"$python_bin" "$repository_root/tools/test_repository_hygiene.py"
"$python_bin" "$repository_root/tools/repository_hygiene.py" --tracked
"$python_bin" "$repository_root/tools/test_control_protocol_static.py"
"$python_bin" "$repository_root/tools/test_firmware_identity_static.py"
"$python_bin" "$repository_root/tools/test_default_hid_safety.py"
"$python_bin" "$repository_root/tools/test_hid_runtime_static.py"
"$python_bin" "$repository_root/tools/test_ble_hid_service_static.py"
"$python_bin" "$repository_root/tools/test_ble_security_static.py"
"$python_bin" "$repository_root/tools/test_uart_control_transport_static.py"
"$python_bin" "$repository_root/tools/test_documentation.py"
"$python_bin" "$repository_root/tools/test_operator_documentation.py"
"$python_bin" "$repository_root/tools/test_firmware_artifact_static.py"
"$python_bin" "$repository_root/tools/test_firmware_artifact_ci_static.py"
"$python_bin" "$repository_root/tools/test_host_artifact.py"
"$python_bin" "$repository_root/tools/test_nonhardware_ci_static.py"
"$python_bin" "$repository_root/tools/test_release_contract.py"
"$python_bin" "$repository_root/tools/test_release_firmware_equality.py"
"$python_bin" "$repository_root/tools/test_release_workflows.py"
"$python_bin" "$repository_root/tools/test_post_flash_provisioning_static.py"
"$repository_root/tools/test-hardware-hid.sh"

echo "PASS: static validation suite"
