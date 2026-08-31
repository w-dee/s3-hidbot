#!/usr/bin/env python3
"""Build the one-wheel U6.4A development artifact in an isolated stage."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from host_artifact import CHECKSUM_BASENAME, WHEEL_BASENAME, checksum_text, sha256_file, validate_artifact_directory


ROOT = Path(__file__).resolve().parents[1]
HOST_INPUT_FILES = ("LICENSE", "MANIFEST.in", "README.md", "pyproject.toml")


def copy_host_source(source_root: Path, stage_root: Path) -> Path:
    source_host = source_root / "host"
    destination = stage_root / "host"
    destination.mkdir()
    for name in HOST_INPUT_FILES:
        shutil.copy2(source_host / name, destination / name)
    shutil.copytree(
        source_host / "src" / "hidbot",
        destination / "src" / "hidbot",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    shutil.copytree(
        source_host / "tests",
        destination / "tests",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--source-root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = args.output_directory.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-host-artifact-") as temporary:
        stage = copy_host_source(args.source_root.resolve(), Path(temporary))
        dist = Path(temporary) / "dist"
        subprocess.run(
            [sys.executable, "-m", "build", "--wheel", "--outdir", str(dist), str(stage)],
            check=True,
        )
        wheels = list(dist.glob("*.whl"))
        if len(wheels) != 1 or wheels[0].name != WHEEL_BASENAME:
            raise RuntimeError("build did not produce exactly the canonical host wheel")
        wheel = wheels[0]
        destination_wheel = output / wheel.name
        shutil.copy2(wheel, destination_wheel)
        (output / CHECKSUM_BASENAME).write_text(
            checksum_text(sha256_file(destination_wheel), destination_wheel.name), encoding="ascii"
        )
    validate_artifact_directory(output)
    print(f"PASS: canonical host artifact {WHEEL_BASENAME}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
