#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_hid_sequence"' EXIT

c++ -std=c++20 -Wall -Wextra -Werror -pedantic \
  -DHID_SEQUENCE_NATIVE_TEST \
  -I"$repository_root/firmware/components/hid_sequence/include" \
  "$repository_root/tools/test_hid_sequence.cpp" \
  "$repository_root/firmware/components/hid_sequence/hid_sequence.cpp" \
  -o "$temporary_directory/test_hid_sequence"

"$temporary_directory/test_hid_sequence"
echo "PASS: bounded HID sequence parser and executor tests"
