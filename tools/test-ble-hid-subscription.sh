#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_binary=$(mktemp "${TMPDIR:-/tmp}/s3-hidbot-ble-hid-subscription.XXXXXX")
trap 'rm -f "$temporary_binary"' EXIT

${CXX:-c++} -std=c++20 -Wall -Wextra -Werror -pthread \
    -I"$repository_root/firmware/components/ble_hid_service/include" \
    "$repository_root/tools/test_ble_hid_subscription.cpp" \
    -o "$temporary_binary"
"$temporary_binary"

echo "PASS: BLE HID subscription state tests"
