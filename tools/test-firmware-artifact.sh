#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

"$python_bin" "$repository_root/tools/test_firmware_artifact.py"

if [[ -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF v5.5.4 must be active for artifact build validation" >&2
    exit 2
fi

source_revision=${S3_HIDBOT_ARTIFACT_TEST_REVISION:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}
source_date_epoch=${S3_HIDBOT_ARTIFACT_TEST_EPOCH:-0}
contract=$("$python_bin" "$repository_root/tools/release_contract.py" --source-root "$repository_root")
artifact_name=$("$python_bin" -c 'import json,sys; print(json.load(sys.stdin)["firmware_archive"])' <<<"$contract")
for artifact in a b; do
    mkdir "$temporary_directory/$artifact"
    "$python_bin" "$repository_root/tools/build_firmware_artifact.py" \
        --output "$temporary_directory/$artifact/$artifact_name" \
        --source-root "$repository_root" \
        --source-revision "$source_revision" \
        --source-date-epoch "$source_date_epoch"
done
cmp "$temporary_directory/a/$artifact_name" "$temporary_directory/b/$artifact_name"
echo "PASS: two isolated firmware artifact builds are byte-identical"
