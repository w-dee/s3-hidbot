#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "ESP-IDF v5.5.4 must be active (IDF_PATH is unset)" >&2
    exit 2
fi
if ! command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF v5.5.4 must be active (idf.py is unavailable)" >&2
    exit 2
fi

idf_version=$(idf.py --version)
if [[ "$idf_version" != *"v5.5.4"* ]]; then
    echo "expected ESP-IDF v5.5.4, got: $idf_version" >&2
    exit 2
fi
grep -Fq "version: 5.5.4" "$repository_root/firmware/dependencies.lock"
grep -Fq "target: esp32s3" "$repository_root/firmware/dependencies.lock"

(
    cd "$repository_root/firmware"
    idf.py build
)

config_env="$repository_root/firmware/build/config.env"
test -f "$config_env"
grep -Fq '"IDF_TARGET": "esp32s3"' "$config_env"
if git -C "$repository_root" ls-files --error-unmatch \
    firmware/build firmware/managed_components >/dev/null 2>&1; then
    echo "generated firmware artifacts must not be tracked" >&2
    exit 1
fi

echo "PASS: ESP-IDF v5.5.4 esp32s3 firmware build"
