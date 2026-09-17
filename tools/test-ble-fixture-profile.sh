#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_fixture_profile"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/hid_capability/include" \
  -I"$repository_root/firmware/components/ble_fixture_profile/include" \
  "$repository_root/tools/test_ble_fixture_profile.cpp" \
  -o "$temporary_directory/test_ble_fixture_profile"
"$temporary_directory/test_ble_fixture_profile"
echo "PASS: finite BLE fixture profile contract"
