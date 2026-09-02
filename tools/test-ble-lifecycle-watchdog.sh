#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_lifecycle_watchdog"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/ble_transport/include" \
  "$repository_root/tools/test_ble_lifecycle_watchdog.cpp" \
  -o "$temporary_directory/test_ble_lifecycle_watchdog"
"$temporary_directory/test_ble_lifecycle_watchdog"
echo "PASS: BLE lifecycle watchdog ownership tests"
