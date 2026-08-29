#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

"$repository_root/tools/test-control-framing.sh"
"$repository_root/tools/test-control-session-lease.sh"
"$repository_root/tools/test-hid-runtime.sh"

echo "PASS: IDF-independent native validation suite"
