#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_control_framing"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" \
  -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/control_framing/include" \
  "$repository_root/tools/test_control_framing.cpp" \
  "$repository_root/firmware/components/control_framing/control_framing.cpp" \
  -o "$temporary_directory/test_control_framing"
"$temporary_directory/test_control_framing"
echo "PASS: control framing and transport sync tests"
