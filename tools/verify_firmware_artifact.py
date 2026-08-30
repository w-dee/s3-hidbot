#!/usr/bin/env python3
"""Verify an extracted firmware bundle or deterministic .tar.gz archive."""

from __future__ import annotations

import argparse
from pathlib import Path

from firmware_artifact import ArtifactError, verify_bundle_archive, verify_bundle_directory


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify an s3-hidbot firmware artifact bundle")
    parser.add_argument("artifact", type=Path, help="bundle directory or .tar.gz archive")
    args = parser.parse_args()
    try:
        if args.artifact.is_dir():
            verify_bundle_directory(args.artifact)
        else:
            verify_bundle_archive(args.artifact)
    except ArtifactError as exc:
        parser.error(str(exc))
    print("PASS: firmware artifact manifest, hashes, flash plan, and privacy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
