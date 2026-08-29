#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_control_session_lease"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/control_session/include" \
  "$repository_root/tools/test_control_session_lease.cpp" \
  "$repository_root/firmware/components/control_session/control_session.cpp" \
  -o "$temporary_directory/test_control_session_lease"
"$temporary_directory/test_control_session_lease"
echo "PASS: control-session lease tests"
