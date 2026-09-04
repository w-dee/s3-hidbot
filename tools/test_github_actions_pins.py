#!/usr/bin/env python3
"""Require immutable full-SHA pins for every external GitHub Action."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github/workflows"
USE = re.compile(r"^\s*-?\s*uses:\s*([^\s#]+)")
EXACT_EXTERNAL = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")


def main() -> int:
    failures: list[str] = []
    external_count = 0
    for workflow in sorted(WORKFLOWS.glob("*.yml")):
        for line_number, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            match = USE.match(line)
            if match is None:
                continue
            reference = match.group(1)
            if reference.startswith("./") or reference.startswith("docker://"):
                continue
            external_count += 1
            if EXACT_EXTERNAL.fullmatch(reference) is None:
                failures.append(f"{workflow.relative_to(ROOT)}:{line_number}: {reference}")
    if failures:
        raise AssertionError("mutable or malformed external action references:\n" + "\n".join(failures))
    if external_count == 0:
        raise AssertionError("no external GitHub Actions references were inspected")
    print(f"PASS: {external_count} external GitHub Actions references use exact 40-hex SHAs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
