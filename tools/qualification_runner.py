#!/usr/bin/env python3
"""Thin non-destructive entrypoint for U7.6 physical qualification preflight."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "tools"))
sys.path.insert(0, str(REPOSITORY / "host" / "src"))

from qualification_harness import (  # noqa: E402
    EvidenceDocument,
    QualificationError,
    artifact_preflight,
    derive_source_identity,
)
from qualification_harness.core import StepResult  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create non-destructive qualification preflight evidence"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--artifact", type=Path)
    preflight.add_argument("--evidence", required=True, type=Path)
    return parser


def run_preflight(args: argparse.Namespace) -> int:
    started = time.monotonic()
    source = derive_source_identity(REPOSITORY)
    artifact = None
    stages: list[dict[str, object]] = [
        {"name": "source_identity", "status": "pass"}
    ]
    if args.artifact is not None:
        artifact = artifact_preflight(args.artifact, source=source)
        stages.append({"name": "artifact_verification", "status": "pass"})
    target = {
        "architecture": "esp32s3" if artifact is None else artifact["firmware"]["target"],
        "profile": None if artifact is None else artifact["firmware"]["build_profile"],
    }
    evidence = EvidenceDocument(
        source=source,
        artifact=artifact,
        target=target,
        started_at=datetime.now(timezone.utc).isoformat(),
        stages=stages,
    ).serialize(
        duration_ms=round((time.monotonic() - started) * 1000),
        main=StepResult("pass"),
        cleanup=StepResult("not_required"),
    )
    with args.evidence.open("x", encoding="utf-8") as stream:
        stream.write(evidence)
    print("QUALIFICATION_PREFLIGHT=PASS")
    print(f"SOURCE_REVISION={source['revision']}")
    print(f"WORKTREE_DIRTY={'YES' if source['dirty'] else 'NO'}")
    print(".envrc checked: NO")
    print(f"S3_HIDBOT_SERIAL resolved: {'YES' if os.environ.get('S3_HIDBOT_SERIAL') else 'NO'}")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "preflight":
            return run_preflight(args)
    except (QualificationError, OSError, subprocess.SubprocessError) as exc:
        print(f"QUALIFICATION_PREFLIGHT=FAIL classification={type(exc).__name__}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
