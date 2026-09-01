#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_usb_lifecycle"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DUSB_LIFECYCLE_NATIVE_TEST \
  -I"$repository_root/firmware/components/usb_lifecycle/include" \
  "$repository_root/tools/test_usb_lifecycle.cpp" \
  "$repository_root/firmware/components/usb_lifecycle/usb_lifecycle.cpp" \
  -o "$temporary_directory/test_usb_lifecycle"
"$temporary_directory/test_usb_lifecycle"
echo "PASS: USB lifecycle desired/observed/safety tests"
