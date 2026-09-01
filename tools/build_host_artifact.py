#!/usr/bin/env python3
"""Build the one-wheel U6.4A development artifact in an isolated stage."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from host_artifact import CHECKSUM_BASENAME, WHEEL_BASENAME, checksum_text, sha256_file, validate_artifact_directory
from host_build import build_host_distributions


ROOT = Path(__file__).resolve().parents[1]
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--source-root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args()
    output = args.output_directory.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    distributions = build_host_distributions(
        args.source_root,
        output,
        wheel=True,
        sdist=False,
        python=sys.executable,
    )
    if len(distributions) != 1 or distributions[0].name != WHEEL_BASENAME:
        raise RuntimeError("build did not produce exactly the canonical host wheel")
    destination_wheel = distributions[0]
    (output / CHECKSUM_BASENAME).write_text(
        checksum_text(sha256_file(destination_wheel), destination_wheel.name), encoding="ascii"
    )
    validate_artifact_directory(output)
    print(f"PASS: canonical host artifact {WHEEL_BASENAME}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
