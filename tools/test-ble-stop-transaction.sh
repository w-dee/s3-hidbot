#!/usr/bin/env bash
set -euo pipefail
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test"; rmdir "$temporary_directory"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic -DBLE_STOP_NATIVE_TEST \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  "$repository_root/tools/test_ble_stop_transaction.cpp" \
  -o "$temporary_directory/test"
"$temporary_directory/test"
echo "PASS: bounded BLE stop transaction ownership and teardown ordering"
