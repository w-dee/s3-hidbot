#!/usr/bin/env python3
"""Require immutable full-SHA pins for every external GitHub Action."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github/workflows"
USE = re.compile(r"^\s*-?\s*uses:\s*([^\s#]+)(?:\s+#\s+(\S+))?")
EXACT_EXTERNAL = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")
APPROVED_ACTIONS = {
    "actions/checkout@93cb6efe18208431cddfb8368fd83d5badbf9bfd": "v5.0.1",
    "actions/setup-python@a309ff8b426b58ec0e2a45f0f869d46889d02405": "v6.2.0",
    "actions/upload-artifact@b7c566a772e6b6bfb58ed0dc250532a479d7789f": "v6.0.0",
    "actions/download-artifact@37930b1c2abaa49bbe596cd826c3c89aef350131": "v7.0.0",
}


def main() -> int:
    failures: list[str] = []
    external_count = 0
    for workflow in sorted(WORKFLOWS.glob("*.yml")):
        for line_number, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            match = USE.match(line)
            if match is None:
                continue
            reference, version_comment = match.groups()
            if reference.startswith("./") or reference.startswith("docker://"):
                continue
            external_count += 1
            if EXACT_EXTERNAL.fullmatch(reference) is None:
                failures.append(f"{workflow.relative_to(ROOT)}:{line_number}: {reference}")
            elif reference not in APPROVED_ACTIONS:
                failures.append(
                    f"{workflow.relative_to(ROOT)}:{line_number}: unapproved pin {reference}"
                )
            elif version_comment != APPROVED_ACTIONS[reference]:
                failures.append(
                    f"{workflow.relative_to(ROOT)}:{line_number}: "
                    f"expected version comment {APPROVED_ACTIONS[reference]} for {reference}"
                )
    if failures:
        raise AssertionError("mutable or malformed external action references:\n" + "\n".join(failures))
    if external_count == 0:
        raise AssertionError("no external GitHub Actions references were inspected")
    print(
        f"PASS: {external_count} external GitHub Actions references use approved exact "
        f"40-hex SHAs across {len(APPROVED_ACTIONS)} Action families"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
