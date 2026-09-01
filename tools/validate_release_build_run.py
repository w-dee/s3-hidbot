#!/usr/bin/env python3
"""Validate immutable GitHub Actions run metadata before draft creation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from release_contract import ReleaseContractError, validate_release_tag, validate_source_revision


class ReleaseBuildRunError(ValueError):
    """A selected Actions run cannot be proven to satisfy its requested evidence kind."""


RELEASE_BUILD_WORKFLOW_PATH = ".github/workflows/release-build.yml"


def _validate_run_common(
    document: dict,
    *,
    repository: str,
    event: str,
    commit: str,
    run_id: str,
) -> None:
    expected_commit = validate_source_revision(commit)
    if not run_id.isdecimal() or int(run_id) <= 0:
        raise ReleaseBuildRunError("run ID must be a positive decimal integer")
    required = {
        "id": int(run_id),
        "name": "Release build",
        "event": event,
        "head_sha": expected_commit,
        "status": "completed",
        "conclusion": "success",
        "path": RELEASE_BUILD_WORKFLOW_PATH,
    }
    for key, value in required.items():
        if document.get(key) != value:
            raise ReleaseBuildRunError(f"release-build run field mismatch: {key}")
    repository_document = document.get("repository")
    if not isinstance(repository_document, dict):
        raise ReleaseBuildRunError("release-build run field mismatch: repository")
    if repository_document.get("full_name") != repository:
        raise ReleaseBuildRunError("release-build run field mismatch: repository.full_name")


def validate_release_build_run(
    document: dict,
    *,
    repository: str,
    tag: str,
    version: str,
    commit: str,
    run_id: str,
) -> None:
    validate_release_tag(tag, version)
    _validate_run_common(
        document,
        repository=repository,
        event="push",
        commit=commit,
        run_id=run_id,
    )
    if document.get("head_branch") != tag:
        raise ReleaseBuildRunError("release-build run field mismatch: head_branch")


def validate_release_candidate_run(
    document: dict,
    *,
    repository: str,
    commit: str,
    run_id: str,
) -> None:
    """Prove a manual candidate run built the same immutable commit."""

    _validate_run_common(
        document,
        repository=repository,
        event="workflow_dispatch",
        commit=commit,
        run_id=run_id,
    )


def validate_release_recovery_run(
    document: dict,
    *,
    repository: str,
    workflow_commit: str,
    run_id: str,
) -> None:
    """Prove a successful manual run used the reviewed recovery-capable workflow."""

    _validate_run_common(
        document,
        repository=repository,
        event="workflow_dispatch",
        commit=workflow_commit,
        run_id=run_id,
    )
    if document.get("head_branch") != "main":
        raise ReleaseBuildRunError("release-build run field mismatch: head_branch")


def _validate_arguments(args: argparse.Namespace, document: dict) -> None:
    if args.kind == "tag":
        if args.tag is None or args.version is None or args.commit is None:
            raise ReleaseBuildRunError("--tag, --version, and --commit are required for a tag run")
        validate_release_build_run(
            document,
            repository=args.repository,
            tag=args.tag,
            version=args.version,
            commit=args.commit,
            run_id=args.run_id,
        )
        return
    if args.kind == "candidate":
        if args.commit is None:
            raise ReleaseBuildRunError("--commit is required for a candidate run")
        validate_release_candidate_run(
            document,
            repository=args.repository,
            commit=args.commit,
            run_id=args.run_id,
        )
        return
    if args.workflow_commit is None:
        raise ReleaseBuildRunError("--workflow-commit is required for a recovery run")
    validate_release_recovery_run(
        document,
        repository=args.repository,
        workflow_commit=args.workflow_commit,
        run_id=args.run_id,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--document", required=True, type=Path)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--kind", choices=("tag", "candidate", "recovery"), required=True)
    parser.add_argument("--tag")
    parser.add_argument("--version")
    parser.add_argument("--commit")
    parser.add_argument("--workflow-commit")
    parser.add_argument("--run-id", required=True)
    args = parser.parse_args()
    try:
        document = json.loads(args.document.read_text(encoding="utf-8"))
        _validate_arguments(args, document)
    except (OSError, json.JSONDecodeError, ReleaseBuildRunError, ReleaseContractError) as exc:
        parser.error(str(exc))
    if args.kind == "recovery":
        print("PASS: requested release-build run used the successful reviewed recovery-capable workflow")
    else:
        print(f"PASS: requested release-build run is the exact successful {args.kind} build")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
