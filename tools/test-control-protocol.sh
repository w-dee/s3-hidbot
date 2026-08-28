#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH must name an active ESP-IDF environment" >&2
  exit 2
fi

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/cjson.o" "$temporary_directory/test_control_protocol"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -I"$IDF_PATH/components/json/cJSON" \
  -c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  -o "$temporary_directory/cjson.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/control_framing/include" \
  -I"$repository_root/firmware/components/control_session/include" \
  -I"$repository_root/firmware/components/control_protocol/include" \
  -I"$IDF_PATH/components/json/cJSON" \
  "$repository_root/tools/test_control_protocol.cpp" \
  "$repository_root/firmware/components/control_framing/control_framing.cpp" \
  "$repository_root/firmware/components/control_session/control_session.cpp" \
  "$repository_root/firmware/components/control_protocol/control_protocol.cpp" \
  "$temporary_directory/cjson.o" \
  -o "$temporary_directory/test_control_protocol"
"$temporary_directory/test_control_protocol"
echo "PASS: control protocol/session tests"
