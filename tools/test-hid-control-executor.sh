#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_hid_control_executor"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_RUNTIME_NATIVE_TEST -DHID_CONTROL_EXECUTOR_NATIVE_TEST -DHID_ROUTE_NATIVE_TEST \
  -DBLE_LIFECYCLE_NATIVE_TEST -DBLE_PAIRING_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_runtime/include" \
  -I"$repository_root/firmware/components/hid_control_executor/include" \
  -I"$repository_root/firmware/components/hid_route/include" \
  -I"$repository_root/firmware/components/usb_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_pairing/include" \
  -I"$repository_root/firmware/components/ble_security/include" \
  "$repository_root/tools/test_hid_control_executor.cpp" \
  "$repository_root/firmware/components/hid_control_executor/hid_control_executor.cpp" \
  "$repository_root/firmware/components/hid_runtime/hid_runtime.cpp" \
  "$repository_root/firmware/components/hid_route/hid_route.cpp" \
  "$repository_root/firmware/components/usb_lifecycle/usb_lifecycle.cpp" \
  "$repository_root/firmware/components/ble_lifecycle/ble_lifecycle.cpp" \
  "$repository_root/firmware/components/ble_pairing/ble_pairing.cpp" \
  "$repository_root/firmware/components/ble_security/ble_security.cpp" \
  -o "$temporary_directory/test_hid_control_executor"
"$temporary_directory/test_hid_control_executor"
echo "PASS: HID control executor tests"
