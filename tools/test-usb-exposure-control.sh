#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_usb_exposure_control"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_RUNTIME_NATIVE_TEST -DUSB_EXPOSURE_CONTROL_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_runtime/include" \
  -I"$repository_root/firmware/components/usb_exposure_control/include" \
  -I"$repository_root/firmware/components/usb_lifecycle/include" \
  "$repository_root/tools/test_usb_exposure_control.cpp" \
  "$repository_root/firmware/components/usb_exposure_control/usb_exposure_control.cpp" \
  "$repository_root/firmware/components/hid_runtime/hid_runtime.cpp" \
  "$repository_root/firmware/components/usb_lifecycle/usb_lifecycle.cpp" \
  -o "$temporary_directory/test_usb_exposure_control"
"$temporary_directory/test_usb_exposure_control"
echo "PASS: USB exposure lifecycle executor tests"
