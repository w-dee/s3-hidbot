#!/usr/bin/env bash
set -euo pipefail
umask 077
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_dir/bootstrap.py" "$@"
