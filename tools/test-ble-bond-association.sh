#!/usr/bin/env bash
set -euo pipefail
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/ble_transport/include" \
  -I"$repository_root/firmware/components/ble_fixture_profile/include" \
  -I"$repository_root/firmware/components/hid_capability/include" \
  -I"$repository_root/firmware/components/ble_security/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  "$repository_root/tools/test_bond_association.cpp" \
  "$repository_root/firmware/components/ble_security/ble_security.cpp" \
  -o "$temporary_directory/test"
"$temporary_directory/test"
"${PYTHON_BIN:-python3}" "$repository_root/tools/test_bond_association_nvs.py"
