#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_ble_route_release_grace_ownership"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_RUNTIME_NATIVE_TEST -DHID_CONTROL_EXECUTOR_NATIVE_TEST \
  -DHID_ROUTE_NATIVE_TEST -DBLE_LIFECYCLE_NATIVE_TEST \
  -DBLE_PAIRING_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_capability/include" \
  -I"$repository_root/firmware/components/ble_fixture_profile/include" \
  -I"$repository_root/firmware/components/hid_runtime/include" \
  -I"$repository_root/firmware/components/hid_route/include" \
  -I"$repository_root/firmware/components/usb_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_lifecycle/include" \
  -I"$repository_root/firmware/components/ble_pairing/include" \
  -I"$repository_root/firmware/components/ble_security/include" \
  -I"$repository_root/firmware/components/secure_memory/include" \
  -I"$repository_root/firmware/components/hid_control_executor/include" \
  -I"$repository_root/firmware/components/ble_transport/include" \
  "$repository_root/tools/test_ble_route_release_grace_ownership.cpp" \
  -o "$temporary_directory/test_ble_route_release_grace_ownership"

"$temporary_directory/test_ble_route_release_grace_ownership"
echo "PASS: bounded BLE route-release grace ownership tests"
