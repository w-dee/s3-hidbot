#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d)
trap 'rm -f "$temporary_directory/test_sensitive_request"; rmdir "$temporary_directory" 2>/dev/null || true' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"$repository_root/firmware/components/sensitive_request/include" \
  -I"$repository_root/firmware/components/secure_memory/include" \
  "$repository_root/tools/test_sensitive_request.cpp" \
  "$repository_root/firmware/components/sensitive_request/sensitive_request.cpp" \
  "$repository_root/firmware/components/secure_memory/secure_memory.cpp" \
  -lcrypto \
  -o "$temporary_directory/test_sensitive_request"
"$temporary_directory/test_sensitive_request"
echo "PASS: sensitive request HMAC tests"
