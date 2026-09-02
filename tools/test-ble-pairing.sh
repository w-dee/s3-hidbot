#!/usr/bin/env bash
set -euo pipefail
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_pairing"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DBLE_PAIRING_NATIVE_TEST \
  -I"$repository_root/firmware/components/ble_pairing/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  "$repository_root/tools/test_ble_pairing.cpp" \
  "$repository_root/firmware/components/ble_pairing/ble_pairing.cpp" \
  -o "$temporary_directory/test_ble_pairing"
"$temporary_directory/test_ble_pairing"
echo "PASS: BLE pairing transaction tests"
