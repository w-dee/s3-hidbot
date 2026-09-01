#!/usr/bin/env python3
"""Render the tracked v0.1.0 release-note source with immutable identity."""

from __future__ import annotations

import argparse
from pathlib import Path

from release_contract import ReleaseContractError, validate_release_tag, validate_source_revision


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-revision", required=True)
    args = parser.parse_args()
    try:
        tag = validate_release_tag(args.tag, args.version)
        revision = validate_source_revision(args.source_revision)
        text = args.template.read_text(encoding="utf-8")
        text = text.replace("<release-tag>", tag).replace("<source-revision>", revision)
        if "<release-tag>" in text or "<source-revision>" in text:
            raise ValueError("release-note template placeholder was not rendered")
        args.output.write_text(text, encoding="utf-8")
    except (OSError, ReleaseContractError, ValueError) as exc:
        parser.error(str(exc))
    print(f"PASS: rendered release notes for {tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
