#!/usr/bin/env bash
set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python_bin=${PYTHON_BIN:-python3}
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

"$python_bin" -m venv "$temporary_directory/venv"
venv_python="$temporary_directory/venv/bin/python"
venv_cli="$temporary_directory/venv/bin/hidbotctl"
package_directory="$temporary_directory/package"

# Install the package as a normal distribution.  Do not rely on the caller's
# site-packages or PYTHONPATH for the import and test checks below.
unset PYTHONPATH
mkdir -p "$package_directory/src"
cp "$repository_root/host/LICENSE" "$repository_root/host/MANIFEST.in" \
    "$repository_root/host/README.md" "$repository_root/host/pyproject.toml" \
    "$package_directory/"
cp -R "$repository_root/host/src/hidbot" "$package_directory/src/hidbot"
"$venv_python" -m pip install --disable-pip-version-check --no-input "$package_directory"
"$venv_python" -c 'import hidbot; import hidbot.client; import hidbot.protocol'
"$venv_cli" --help >/dev/null

(
    cd "$repository_root"
    "$venv_python" -m unittest discover -s host/tests
)

echo "PASS: host install/import/CLI/unit tests"
