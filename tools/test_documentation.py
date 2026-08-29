#!/usr/bin/env python3
"""Check stable repository documentation navigation invariants."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
MARKDOWN_LINK = re.compile(r"\[[^\]]*\]\(([^)]*)\)")
FENCED_BLOCK = re.compile(r"```[^\n]*\n(.*?)```", re.DOTALL)
TOOL_COMMAND = re.compile(r"^\s*(\./tools/[A-Za-z0-9_.-]+\.sh)\s*$", re.MULTILINE)


def _git(*arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(ROOT), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def _tracked_markdown() -> list[Path]:
    return [ROOT / name for name in _git("ls-files", "--", "*.md").splitlines()]


def _relative_link_target(source: Path, raw_target: str) -> Path | None:
    target = raw_target.strip()
    if target.startswith("<") and ">" in target:
        target = target[1 : target.index(">")]
    else:
        target = target.split(maxsplit=1)[0]

    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc or target.startswith("#"):
        return None
    if not parsed.path:
        return None
    if parsed.path.startswith("/"):
        raise ValueError("absolute link path is not repository-relative")

    candidate = (source.parent / unquote(parsed.path)).resolve()
    try:
        candidate.relative_to(ROOT)
    except ValueError as exc:
        raise ValueError("link escapes the repository") from exc
    return candidate


def _check_markdown_links(errors: list[str]) -> int:
    count = 0
    for source in _tracked_markdown():
        text = source.read_text(encoding="utf-8")
        for match in MARKDOWN_LINK.finditer(text):
            try:
                target = _relative_link_target(source, match.group(1))
            except ValueError as exc:
                errors.append(f"{source.relative_to(ROOT)}: {exc}")
                continue
            if target is None:
                continue
            count += 1
            if not target.exists():
                errors.append(
                    f"{source.relative_to(ROOT)}: missing relative link target "
                    f"{target.relative_to(ROOT)}"
                )
    return count


def _check_validation_scripts(errors: list[str]) -> int:
    validation_doc = ROOT / "docs/development/validation-entrypoints.md"
    text = validation_doc.read_text(encoding="utf-8")
    commands: set[str] = set()
    for block in FENCED_BLOCK.findall(text):
        commands.update(TOOL_COMMAND.findall(block))
    if not commands:
        errors.append(f"{validation_doc.relative_to(ROOT)}: no tool entrypoints found")
        return 0

    for command in sorted(commands):
        relative = command[2:]
        path = ROOT / relative
        if not path.is_file():
            errors.append(f"{validation_doc.relative_to(ROOT)}: missing {command}")
            continue
        stage_lines = _git("ls-files", "--stage", "--", relative).splitlines()
        if not stage_lines:
            errors.append(f"{relative}: tool is not tracked")
            continue
        mode = stage_lines[0].split(maxsplit=1)[0]
        if mode != "100755":
            errors.append(f"{relative}: expected tracked mode 100755, got {mode}")
    return len(commands)


def main() -> int:
    errors: list[str] = []
    link_count = _check_markdown_links(errors)
    script_count = _check_validation_scripts(errors)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(
        f"PASS: documentation guard ({link_count} relative links, "
        f"{script_count} validation scripts)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
