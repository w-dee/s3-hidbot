#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

"${CXX:-c++}" \
  -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DFIRMWARE_IDENTITY_NATIVE_TEST \
  -I"$repository_root/firmware/components/firmware_identity/include" \
  "$repository_root/tools/test_firmware_identity.cpp" \
  "$repository_root/firmware/components/firmware_identity/firmware_identity.cpp" \
  -o "$temporary_directory/test_firmware_identity"
"$temporary_directory/test_firmware_identity"
echo "PASS: firmware identity encoder and validator tests"
