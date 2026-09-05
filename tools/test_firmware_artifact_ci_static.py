#!/usr/bin/env python3
"""Static guards for the dedicated U6.3B firmware artifact workflow."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/firmware-artifact.yml"
EXPECTED_CONTAINER = (
    "espressif/idf:v5.5.4@sha256:"
    "b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b"
)
CHECKOUT_SHA = "93cb6efe18208431cddfb8368fd83d5badbf9bfd"
UPLOAD_SHA = "b7c566a772e6b6bfb58ed0dc250532a479d7789f"


def _required(text: str, pattern: str, description: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise AssertionError(f"missing workflow contract: {description}")


def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8")
    _required(text, r"^name:\s*Firmware artifact\s*$", "dedicated workflow name")
    for trigger in ("push:", "pull_request:", "workflow_dispatch:"):
        _required(text, rf"^\s*{re.escape(trigger)}\s*$", trigger)
    _required(text, r"^permissions:\s*\n\s+contents:\s*read\s*$", "read-only permissions")
    _required(text, r"^concurrency:\s*$", "concurrency")
    _required(text, r"cancel-in-progress:\s*true", "run cancellation")
    _required(text, r"timeout-minutes:\s*60", "bounded job timeout")

    if EXPECTED_CONTAINER not in text:
        raise AssertionError("workflow container does not use the verified immutable digest")
    _required(text, r"ESP_IDF_CONTAINER_IMAGE:\s*&esp_idf_container_image", "container identity anchor")
    _required(text, r"image:\s*\*esp_idf_container_image", "container identity reuse")
    if text.count(EXPECTED_CONTAINER) != 1:
        raise AssertionError("container digest must have one source value")
    _required(text, r"--container-image\s+\"\$ESP_IDF_CONTAINER_IMAGE\"", "manifest container provenance")

    _required(text, rf"actions/checkout@{CHECKOUT_SHA}", "immutable checkout action")
    _required(text, rf"actions/upload-artifact@{UPLOAD_SHA}", "immutable upload action")
    if re.search(r"actions/[A-Za-z0-9_.-]+@v[0-9]", text):
        raise AssertionError("dedicated workflow must not use mutable action tags")

    _required(text, r"EXPECTED_SHA:\s*\$\{\{\s*github\.sha\s*\}\}", "exact GitHub source revision")
    _required(
        text,
        r'git\s+-c\s+"safe\.directory=\$GITHUB_WORKSPACE"\s+rev-parse\s+HEAD',
        "workspace-scoped checkout identity check",
    )
    _required(
        text,
        r'git\s+-c\s+"safe\.directory=\$GITHUB_WORKSPACE"\s+\\\s*\n\s*show\s+-s\s+--format=%ct',
        "workspace-scoped commit timestamp derivation",
    )
    if re.search(r"safe\.directory\s*=\s*\*|safe\.directory=/__w/|chown\s+-R|chmod\s+-R", text):
        raise AssertionError("workflow must not use wildcard or persistent ownership workarounds")
    if re.search(r"git\s+config\s+--(?:global|system)|GIT_CONFIG_(?:GLOBAL|SYSTEM)", text):
        raise AssertionError("workflow must not use persistent global/system Git configuration")
    static_step = text.split(
        "      - name: Run artifact static and privacy guards\n", 1
    )[1].split("      - name: Build and verify two independent artifacts\n", 1)[0]
    _required(static_step, r"GIT_CONFIG_COUNT:\s*[\"']?1[\"']?", "nested Git config count")
    _required(static_step, r"GIT_CONFIG_KEY_0:\s*safe\.directory", "nested Git safe-directory key")
    _required(static_step, r"GIT_CONFIG_VALUE_0:\s*\$\{\{\s*github\.workspace\s*\}\}", "trusted workspace value")
    build_step = text.split(
        "      - name: Build and verify two independent artifacts\n", 1
    )[1]
    if re.search(r"GIT_CONFIG_(?:COUNT|KEY_0|VALUE_0)", build_step):
        raise AssertionError("nested Git safe-directory environment must not reach artifact builds")
    _required(text, r"SOURCE_DATE_EPOCH", "explicit SOURCE_DATE_EPOCH")
    _required(text, r"S3_HIDBOT_SOURCE_REVISION", "explicit source revision")
    _required(text, r"tools/release_contract\.py", "derived future artifact name")
    if "0.1.0" in text:
        raise AssertionError("generic firmware artifact workflow must not hardcode a product version")
    _required(text, r"tools/privacy_lint\.py\s+--tracked", "repository privacy scan")
    _required(text, r"tools/test_firmware_artifact\.py", "artifact privacy and verifier tests")

    _required(text, r"build_one\(\)\s*\{", "canonical builder function")
    if text.count("tools/build_firmware_artifact.py") != 1:
        raise AssertionError("workflow should invoke the canonical builder, not duplicate it")
    if text.count("tools/verify_firmware_artifact.py") != 2:
        raise AssertionError("both independent bundles must use the official verifier")
    _required(text, r'build_one\s+"\$artifact_root/a/', "independent build A")
    _required(text, r'build_one\s+"\$artifact_root/b/', "independent build B")
    if text.count("runs-on:") != 1 or text.count("container:") != 1:
        raise AssertionError("resource enforcement must not add a job or container")
    if "resource-gate" in text or "resource_gate" in text:
        raise AssertionError("resource enforcement belongs inside the canonical builder")
    _required(text, r"sha256sum", "archive hash comparison")
    _required(text, r"\n\s+cmp\s+\"\$artifact_root/a/", "byte-identical archive comparison")
    _required(text, r"find \"\$artifact_root/extract-a\"", "payload comparison")
    _required(text, r"cmp \"\$artifact_root/extract-a/\$relative\"", "payload byte comparison")

    _required(text, r"name:\s*firmware-artifact", "Actions artifact name")
    _required(text, r"retention-days:\s*14", "temporary artifact retention")
    _required(text, r"canonical/\$\{\{ env\.ARTIFACT_NAME \}\}", "single canonical archive upload")
    _required(text, r"sha256sum \"\$artifact_root/canonical/\$ARTIFACT_NAME\"", "outer archive checksum")
    if text.count("actions/upload-artifact@") != 1:
        raise AssertionError("workflow must upload exactly one Actions artifact")
    if "artifact_root/b/$ARTIFACT_NAME" in text.split("path:", 1)[-1]:
        raise AssertionError("build B must not be uploaded")

    forbidden = (
        "actions/create-release",
        "softprops/action-gh-release",
        "gh release",
        "git tag",
        "esptool.py write_flash",
        "idf.py flash",
        "/dev/tty",
        "S3_HIDBOT_SERIAL",
        "--hardware",
    )
    for marker in forbidden:
        if marker in text:
            raise AssertionError(f"forbidden physical/publication operation in workflow: {marker}")

    print("PASS: dedicated firmware artifact workflow static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
