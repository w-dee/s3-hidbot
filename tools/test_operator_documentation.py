#!/usr/bin/env python3
"""Focused drift guards for the external operator contract."""

from __future__ import annotations

import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPERATOR = ROOT / "docs/operator"
CLI_SOURCE = ROOT / "host/src/hidbot/cli.py"


def _command_names() -> set[str]:
    """Extract public parser names without importing pyserial-dependent code."""

    tree = ast.parse(CLI_SOURCE.read_text(encoding="utf-8"), filename=str(CLI_SOURCE))
    names: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr != "add_parser" or not node.args:
            continue
        first = node.args[0]
        if isinstance(first, ast.Constant) and isinstance(first.value, str):
            names.add(first.value)
    for node in ast.walk(tree):
        if not isinstance(node, ast.For) or not isinstance(node.iter, ast.Tuple):
            continue
        if not (
            isinstance(node.target, ast.Tuple)
            and len(node.target.elts) == 2
            and isinstance(node.target.elts[0], ast.Name)
            and node.target.elts[0].id == "name"
            and any(
                isinstance(child, ast.Call)
                and isinstance(child.func, ast.Attribute)
                and child.func.attr == "add_parser"
                and isinstance(child.func.value, ast.Name)
                and child.func.value.id == "commands"
                for child in ast.walk(node)
            )
        ):
            continue
        for item in node.iter.elts:
            if not isinstance(item, ast.Tuple) or not item.elts:
                continue
            first = item.elts[0]
            if isinstance(first, ast.Constant) and isinstance(first.value, str):
                names.add(first.value)
    return names


def _require(text: str, value: str, description: str) -> None:
    if value not in text:
        raise AssertionError(f"missing operator documentation: {description}")


def main() -> int:
    required_paths = {
        "README.md",
        "quick-start.md",
        "cli-reference.md",
        "safety-and-recovery.md",
        "automation.md",
    }
    actual_paths = {path.name for path in OPERATOR.glob("*.md")}
    assert required_paths <= actual_paths, "operator documentation hierarchy is incomplete"

    documents = {
        path.name: path.read_text(encoding="utf-8")
        for path in sorted(OPERATOR.glob("*.md"))
    }
    all_text = "\n".join(documents.values())
    cli = documents["cli-reference.md"]
    quick_start = documents["quick-start.md"]
    safety = documents["safety-and-recovery.md"]
    automation = documents["automation.md"]

    for command in _command_names():
        _require(cli, f"`{command}", f"public command {command!r} in CLI reference")

    for command in ("keyboard-report", "mouse-report"):
        _require(cli, command, f"unsafe command {command}")
    _require(cli, "Unsafe HID injection", "unsafe command taxonomy")
    _require(cli, "--unsafe-hid", "unsafe HID opt-in")
    _require(safety, "--unsafe-hid", "unsafe HID recovery rule")

    _require(cli, "Hardware-free validation", "hardware-free taxonomy")
    _require(cli, "No serial, HID, or hardware access", "verify-artifact isolation")
    assert quick_start.index("verify-artifact") < quick_start.index("flash-firmware"), (
        "quick start must verify an artifact before flash-firmware"
    )
    _require(safety, "never automatically reflashes", "no reflash after programming")

    for code in ("0", "2", "3", "4", "5", "6", "7", "8", "130"):
        _require(cli, f"| {code}", f"exit code {code}")
    _require(cli, "S3_HIDBOT_SERIAL", "serial environment variable")
    _require(cli, "S3_HIDBOT_BAUD", "baud environment variable")
    _require(cli, "ignores `S3_HIDBOT_BAUD`", "flash baud exception")
    _require(cli, "`ok:true` with `match:false`", "JSON ok-versus-match distinction")

    _require(all_text, "PyPI", "PyPI deferral")
    _require(all_text, "GitHub Releases", "stable release acquisition")
    _require(all_text, "not stable releases", "development artifact distinction")
    assert "v0.1.0 is published" not in all_text.lower(), (
        "source documentation must not claim the pending version is already published"
    )
    _require(all_text, "14 days", "Actions retention")
    _require(quick_start, "<ch343-control-port>", "serial placeholder")
    assert "/dev/" not in all_text, "operator docs must not contain a machine-local serial path"

    for unknown in (
        "VBUS sourcing",
        "backfeed",
        "dual-cable power safety",
        "detach sensing",
        "VBUS monitoring",
    ):
        _require(all_text, unknown, f"explicit unknown: {unknown}")
    _require(all_text, "**UNKNOWN**", "explicit hardware-unknown marker")
    _require(all_text, "Linux-only", "physical validation platform limit")

    _require(automation, "stdout JSON", "automation JSON guidance")
    _require(automation, "FLASHED_VERIFICATION_FAILED", "phase-aware flash failure")
    _require(automation, "Never automatically erase flash", "agent flash prohibition")
    _require(automation, "Never commit", "agent machine-local configuration prohibition")
    print(f"PASS: operator documentation guard ({len(_command_names())} public commands)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
