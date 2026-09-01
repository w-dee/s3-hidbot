#!/usr/bin/env python3
"""Assemble the exact public-release asset set without publishing it."""

from __future__ import annotations

import argparse
from pathlib import Path

from release_assets import ReleaseAssetError, prepare_release_assets
from release_contract import ReleaseContractError, read_release_contract


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--firmware-artifact", required=True, type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source-revision", help="require the firmware bundle to use this commit")
    args = parser.parse_args()
    try:
        contract = read_release_contract(args.source_root)
        prepare_release_assets(
            args.output,
            args.source_root.resolve(),
            args.firmware_artifact,
            contract,
            source_revision=args.source_revision,
        )
    except (ReleaseAssetError, ReleaseContractError, OSError) as exc:
        parser.error(str(exc))
    print("PASS: prepared exact temporary release asset set")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
