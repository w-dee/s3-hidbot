#!/usr/bin/env python3
"""Fail closed if the release-candidate dependency graph loses exact Git pins."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = (ROOT / "firmware/main/idf_component.yml").read_text(encoding="utf-8")
LOCK = (ROOT / "firmware/dependencies.lock").read_text(encoding="utf-8")

ESP_USB_COMMIT = "94a4d44b5760b8f6ab1a3ce56c92a101fe2bc17f"
ESP_USB_HASH = "43b8cbff24d95d939240e2f424970b597f6732cdee95f15f8d6cb494cd914fe0"
TINYUSB_COMMIT = "0b02e68af7a654d5099d8a230291ce19403833ae"
TINYUSB_HASH = "06494427af49510651de7d0935b449a27fdd35ae1cfdd1361a732d4877263223"


def dependency_block(name: str, next_name: str) -> str:
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  {re.escape(next_name)}:)",
        LOCK,
        re.MULTILINE | re.DOTALL,
    )
    assert match is not None, f"missing lock entry: {name}"
    return match.group(1)


assert "overrides:" not in MANIFEST
for required in (
    "  espressif/esp_tinyusb:",
    "    git: https://github.com/w-dee/esp-usb.git",
    "    path: device/esp_tinyusb",
    f"    version: {ESP_USB_COMMIT}",
):
    assert required in MANIFEST, f"missing exact manifest dependency: {required}"

esp_tinyusb = dependency_block("espressif/esp_tinyusb", "espressif/tinyusb")
for required in (
    f"component_hash: {ESP_USB_HASH}",
    "git: https://github.com/w-dee/esp-usb.git",
    "path: device/esp_tinyusb",
    "type: git",
    f"version: {ESP_USB_COMMIT}",
    "name: espressif/tinyusb",
    f"version: {TINYUSB_COMMIT}",
):
    assert required in esp_tinyusb, f"unexpected esp_tinyusb lock entry: {required}"

tinyusb = dependency_block("espressif/tinyusb", "idf")
for required in (
    f"component_hash: {TINYUSB_HASH}",
    "git: https://github.com/w-dee/tinyusb.git",
    "path: .",
    "type: git",
    f"version: {TINYUSB_COMMIT}",
):
    assert required in tinyusb, f"unexpected TinyUSB lock entry: {required}"

assert "registry_url" not in esp_tinyusb
assert "registry_url" not in tinyusb
assert "- espressif/esp_tinyusb\n- idf\n" in LOCK

print("PASS: immutable two-level esp_tinyusb and TinyUSB Git dependency graph")
