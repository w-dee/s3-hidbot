#!/usr/bin/env python3
"""Verify an already-downloaded temporary release asset set."""

from __future__ import annotations

import argparse
from pathlib import Path

from release_assets import ReleaseAssetError, validate_release_asset_directory
from release_contract import ReleaseContractError, read_release_contract


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset_directory", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source-revision", help="require the firmware bundle to use this commit")
    args = parser.parse_args()
    try:
        contract = read_release_contract(args.source_root)
        manifest = validate_release_asset_directory(
            args.asset_directory,
            contract,
            source_revision=args.source_revision,
        )
    except (ReleaseAssetError, ReleaseContractError, OSError) as exc:
        parser.error(str(exc))
    print(
        "PASS: exact release asset set verified "
        f"({manifest['firmware']['version']} {manifest['firmware']['source_revision']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
