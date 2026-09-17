#!/usr/bin/env bash
set -euo pipefail
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_security" "$temporary_directory/test_bond_delete"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/hid_capability/include" \
  -I"$repository_root/firmware/components/ble_fixture_profile/include" \
  -I"$repository_root/firmware/components/ble_security/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_transport" \
  "$repository_root/tools/test_ble_security.cpp" \
  "$repository_root/firmware/components/ble_security/ble_security.cpp" \
  -o "$temporary_directory/test_ble_security"
"$temporary_directory/test_ble_security"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/hid_capability/include" \
  -I"$repository_root/firmware/components/ble_fixture_profile/include" \
  -I"$repository_root/firmware/components/ble_security/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_transport" \
  "$repository_root/tools/test_bond_delete_transaction.cpp" \
  -o "$temporary_directory/test_bond_delete"
"$temporary_directory/test_bond_delete"
"${PYTHON_BIN:-python3}" "$repository_root/tools/test_bond_delete_nvs.py"
echo "PASS: BLE security foundation tests"
