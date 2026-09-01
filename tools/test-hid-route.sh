#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_hid_route"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_ROUTE_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_route/include" \
  "$repository_root/tools/test_hid_route.cpp" \
  "$repository_root/firmware/components/hid_route/hid_route.cpp" \
  -o "$temporary_directory/test_hid_route"
"$temporary_directory/test_hid_route"
echo "PASS: HID route generation tests"
