#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_hid_runtime"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_RUNTIME_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_runtime/include" \
  "$repository_root/tools/test_hid_runtime.cpp" \
  "$repository_root/firmware/components/hid_runtime/hid_runtime.cpp" \
  -o "$temporary_directory/test_hid_runtime"
"$temporary_directory/test_hid_runtime"
echo "PASS: HID runtime lifecycle/state/safety tests"
