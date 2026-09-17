#!/usr/bin/env python3
"""Focused drift guards for the external operator contract."""

from __future__ import annotations

import ast
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OPERATOR = ROOT / "docs/operator"
CLI_SOURCE = ROOT / "host/src/hidbot/cli.py"
README = ROOT / "README.md"
HARDWARE_VALIDATION = ROOT / "docs/development/hardware-validation.md"
HARDWARE_PROFILE_ERRATUM = ROOT / "docs/development/hardware-profile-erratum.md"
UART_CONTROL_PLANE = ROOT / "docs/development/uart-control-plane.md"
FIRMWARE_ARTIFACTS = ROOT / "docs/development/firmware-artifacts.md"
HOST_README = ROOT / "host/README.md"
RELEASE_NOTES = ROOT / "docs/development/release-notes-v0.1.0.md"
RELEASE_NOTES_RENDERER = ROOT / "tools/render_release_notes.py"
PUBLISHED_RELEASE_NOTES = ROOT / "docs/development/release-notes-v0.2.0.md"
IDENTIFIER_QUALIFICATION_MARKERS = (
    "project-specific USB-IF VID/PID assignment",
    "project-specific Bluetooth SIG Company Identifier",
    "Bluetooth product qualification or listing",
    "v0.1.0 does not implement BLE HID",
    "development and interoperability testing",
    "production or commercial identifier allocation",
    "Anyone incorporating, redistributing, manufacturing, selling, or otherwise using",
    "existing MIT License does not add a non-commercial-use restriction",
)


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


def _require_semantic_marker(text: str, value: str, description: str) -> None:
    if " ".join(value.split()) not in " ".join(text.split()):
        raise AssertionError(f"missing operator documentation: {description}")


def _render_release_notes() -> str:
    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary) / "release-notes.md"
        subprocess.run(
            [
                sys.executable,
                str(RELEASE_NOTES_RENDERER),
                "--template",
                str(RELEASE_NOTES),
                "--output",
                str(output),
                "--tag",
                "v0.1.0",
                "--version",
                "0.1.0",
                "--source-revision",
                "a" * 40,
            ],
            check=True,
        )
        return output.read_text(encoding="utf-8")


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
    readme = README.read_text(encoding="utf-8")
    hardware_validation = HARDWARE_VALIDATION.read_text(encoding="utf-8")
    hardware_profile_erratum = HARDWARE_PROFILE_ERRATUM.read_text(encoding="utf-8")
    uart_control_plane = UART_CONTROL_PLANE.read_text(encoding="utf-8")
    firmware_artifacts = FIRMWARE_ARTIFACTS.read_text(encoding="utf-8")
    host_readme = HOST_README.read_text(encoding="utf-8")
    release_notes = RELEASE_NOTES.read_text(encoding="utf-8")
    published_release_notes = PUBLISHED_RELEASE_NOTES.read_text(encoding="utf-8")

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
    _require(all_text, "Linux-first", "physical validation platform limit")
    fixture_text = readme + "\n" + all_text + "\n" + hardware_validation
    for marker in (
        "Freenove FNK0099 ESP32-S3 WROOM Board Lite",
        "ESP32-S3-WROOM-1",
        "non-destructive query",
        "8 MiB flash",
        "8 MiB embedded PSRAM",
        "N8R8 configuration",
        "minimum 4 MiB flash",
    ):
        _require(fixture_text, marker, f"corrected board/support scope {marker}")
    _require(readme + all_text, "does not require external PSRAM", "external PSRAM is optional")
    _require(
        hardware_validation,
        "This is a measurement of the fixture, not a claim",
        "measured fixture capacity scope",
    )
    _require(
        hardware_validation,
        "about every FNK0099 variant.",
        "FNK0099 variant capacity limit",
    )
    _require(
        hardware_validation,
        "DIRECT_PHYSICAL_OBSERVATION_ON_FNK0099",
        "observation-scoped VBUS provenance",
    )
    for limit in ("not a schematic", "backfeed behavior", "dual-supply safety"):
        _require(hardware_validation, limit, f"VBUS evidence limit {limit!r}")
    for marker in (
        "monochrome blue onboard LED on GPIO2",
        "`HIGH` is on and `LOW` is off",
        "WS2812/NeoPixel on GPIO48",
    ):
        _require(hardware_validation, marker, f"FNK0099 LED fact {marker!r}")
    for marker in (
        "same physical fixture",
        "does not qualify every FNK0099 unit or board revision",
        "creates no qualification evidence for actual FNK0085 hardware",
        "Frozen evidence objects",
        "freenove-fnk0085",
        "Forward source now emits `freenove-fnk0099`",
        "the two profile values are not aliases",
    ):
        _require(hardware_profile_erratum, marker, f"fixture erratum scope {marker!r}")
    for document, markers in (
        (
            quick_start,
            (
                "freenove-fnk0099.tar.gz.sha256",
                "Published historical artifacts retain their literal old names",
            ),
        ),
        (
            safety,
            (
                "--allow-legacy-v0-3-0-recovery",
                "fixed outer and internal hashes",
                "runtime verification still requires its literal",
            ),
        ),
        (
            firmware_artifacts,
            (
                "build_profile=freenove-fnk0099",
                "s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0099/",
                "Read-only inspection",
                "Ordinary old-profile artifacts are rejected",
            ),
        ),
        (
            uart_control_plane,
            (
                '"build_profile": "freenove-fnk0099"',
                "does not alias `freenove-fnk0085` to `freenove-fnk0099`",
            ),
        ),
        (
            host_readme,
            (
                "--allow-legacy-v0-3-0-recovery",
                "does not authorize an extracted",
            ),
        ),
    ):
        for marker in markers:
            _require(document, marker, f"forward/legacy contract {marker!r}")
    maintained_fixture_prose = "\n".join(
        (
            readme,
            all_text,
            hardware_validation,
            uart_control_plane,
            firmware_artifacts,
            host_readme,
        )
    )
    for wrong_claim in (
        "physically qualified fixture is the **Freenove ESP32-S3 WROOM Board /\nFNK0085",
        "Only the Freenove ESP32-S3 WROOM Board / FNK0085",
        "need a Freenove ESP32-S3 WROOM Board / FNK0085",
        "FNK0085 USB connectors share the board power rail",
    ):
        if wrong_claim in maintained_fixture_prose:
            raise AssertionError(f"obsolete physical-fixture claim remains: {wrong_claim!r}")
    for obsolete in (
        "current pre-H-contract runtime",
        "current legacy artifact identifier",
        "Future FNK0099 naming is not implemented",
        "Until H-contract it accepts",
    ):
        if obsolete in maintained_fixture_prose:
            raise AssertionError(f"obsolete pre-H-contract statement remains: {obsolete!r}")

    _require(documents["README.md"], "Route v2", "current BLE route contract")
    _require(documents["README.md"], "never evicts", "three-bond no-eviction contract")
    for marker in ("authenticated pairing", "16-byte key", 'last_result:"store_full"'):
        _require(cli + all_text, marker, f"BLE security contract {marker}")
    for marker in (
        "Supported safe/quiescent state",
        "Operator-visible error taxonomy",
        "SESSION_MISMATCH",
        "BLE_BOND_STORAGE",
        "must not be blindly retried",
    ):
        _require(safety, marker, f"safe-state/error contract {marker}")

    current_facing = readme + "\n" + all_text
    obsolete = re.compile(
        r"(?:BLE HID output|BLE notification|pairing|bonding|multi-peer).{0,60}"
        r"(?:not implemented|not yet implemented|remain out of scope)",
        re.IGNORECASE,
    )
    for paragraph in re.split(r"\n\s*\n", current_facing):
        if obsolete.search(paragraph) and not re.search(
            r"v0\.1\.0|historical|earlier milestone", paragraph, re.IGNORECASE
        ):
            raise AssertionError(f"obsolete current-state claim is not historicalized: {paragraph!r}")

    for marker in (
        "Status: **PUBLISHED**",
        "releases/tag/v0.2.0",
        "a2522a90b6ad58fdfb075f6325102b576b1d636e",
        "b2f87b8c52e155d1bd4acf0bc765f33dad172dfc",
        "33913669093",
        "exactly 10 public assets",
        "immutable v0.2.0 tag retains the pre-publication version",
    ):
        _require(published_release_notes, marker, f"published v0.2.0 history {marker}")
    for stale_marker in (
        "unreleased draft",
        "PREPARATION ONLY",
        "remain pending",
        "subsequent authorized qualification gates",
    ):
        if stale_marker in published_release_notes:
            raise AssertionError(f"stale v0.2.0 release-note wording: {stale_marker}")

    _require(automation, "stdout JSON", "automation JSON guidance")
    _require(automation, "FLASHED_VERIFICATION_FAILED", "phase-aware flash failure")
    _require(automation, "Never automatically erase flash", "agent flash prohibition")
    _require(automation, "Never commit", "agent machine-local configuration prohibition")

    _require(
        documents["README.md"],
        "safety-and-recovery.md#external-identifiers-qualification-and-distribution-responsibility",
        "operator link to identifier and qualification statement",
    )
    for document_name, text in {
        "repository README": readme,
        "operator safety documentation": safety,
        "release-note template": release_notes,
        "rendered release notes": _render_release_notes(),
    }.items():
        for marker in IDENTIFIER_QUALIFICATION_MARKERS:
            _require_semantic_marker(text, marker, f"{document_name} marker {marker!r}")

    print(f"PASS: operator documentation guard ({len(_command_names())} public commands)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
