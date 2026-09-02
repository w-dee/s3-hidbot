#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_lifecycle"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DBLE_LIFECYCLE_NATIVE_TEST \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  "$repository_root/tools/test_ble_lifecycle.cpp" \
  "$repository_root/firmware/components/ble_lifecycle/ble_lifecycle.cpp" \
  -o "$temporary_directory/test_ble_lifecycle"
"$temporary_directory/test_ble_lifecycle"
echo "PASS: BLE lifecycle tests"
