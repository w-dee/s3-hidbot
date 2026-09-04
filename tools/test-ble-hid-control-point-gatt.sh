#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/ble_hid_service.o" "$temporary_directory/test.o" "$temporary_directory/test_ble_hid_control_point_gatt"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

# The fake constants and layouts were audited against ESP-IDF v5.5.4. Make an
# IDF upgrade stop here until that audit and the fake version pin move together.
# This remains IDF-independent and does not source local setup.
idf_pin_count=0
while IFS= read -r line; do
    if [[ "$line" == "    version: 5.5.4" ]]; then
        ((idf_pin_count += 1))
    fi
done < "$repository_root/firmware/dependencies.lock"
if [[ $idf_pin_count != 1 ]]; then
    echo "U7.6A NimBLE fake requires the repository IDF lock at 5.5.4" >&2
    exit 1
fi

cxx=${CXX:-c++}
compile_flags=(
    -std=c++20 -Wall -Wextra -Werror -pedantic
    -DS3_HIDBOT_U76A_NIMBLE_FAKE
    -DHID_RUNTIME_NATIVE_TEST -DHID_CONTROL_EXECUTOR_NATIVE_TEST
    -DHID_ROUTE_NATIVE_TEST -DBLE_LIFECYCLE_NATIVE_TEST
    -DBLE_PAIRING_NATIVE_TEST
    -I"$repository_root/tools/fakes/esp-idf-v5.5.4-nimble"
    -I"$repository_root/firmware/components/ble_hid_service/include"
    -I"$repository_root/firmware/components/hid_control_executor/include"
    -I"$repository_root/firmware/components/hid_runtime/include"
    -I"$repository_root/firmware/components/hid_route/include"
    -I"$repository_root/firmware/components/usb_lifecycle/include"
    -I"$repository_root/firmware/components/ble_lifecycle/include"
    -I"$repository_root/firmware/components/ble_pairing/include"
    -I"$repository_root/firmware/components/ble_security/include"
    -I"$repository_root/firmware/components/secure_memory/include"
)

"$cxx" "${compile_flags[@]}" -Wno-missing-field-initializers \
    -c "$repository_root/firmware/components/ble_hid_service/ble_hid_service.cpp" \
    -o "$temporary_directory/ble_hid_service.o"
"$cxx" "${compile_flags[@]}" \
    -c "$repository_root/tools/test_ble_hid_control_point_gatt.cpp" \
    -o "$temporary_directory/test.o"
"$cxx" "$temporary_directory/ble_hid_service.o" \
    "$temporary_directory/test.o" \
    -o "$temporary_directory/test_ble_hid_control_point_gatt"

"$temporary_directory/test_ble_hid_control_point_gatt"
