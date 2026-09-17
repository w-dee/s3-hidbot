#!/usr/bin/env python3
"""Prove the controller grace race tests kill both former check/use defects."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "firmware/components/hid_control_executor/hid_control_executor.cpp"

MUTATIONS = {
    "claim-current-after-validation": (
        "    const auto claim = ble_route_grace_authority_.claim(candidate);\n",
        "    candidate = ble_route_grace_authority_.find_armed(\n"
        "        ble_route_release_);\n"
        "    const auto claim = ble_route_grace_authority_.claim(candidate);\n",
    ),
    "publish-current-after-old-claim": (
        "    if (!ble_route_grace_authority_.publish_due(claim)) {\n",
        "    const auto current_candidate =\n"
        "        ble_route_grace_authority_.find_armed(ble_route_release_);\n"
        "    const auto current_claim =\n"
        "        ble_route_grace_authority_.claim(current_candidate);\n"
        "    if (!ble_route_grace_authority_.publish_due(\n"
        "            current_claim.valid() ? current_claim : claim)) {\n",
    ),
}


def compile_mutation(source: Path, output: Path) -> None:
    include_components = (
        "hid_capability",
        "ble_fixture_profile",
        "hid_runtime",
        "ble_hid_service",
        "hid_control_executor",
        "hid_route",
        "usb_lifecycle",
        "ble_lifecycle",
        "ble_pairing",
        "ble_security",
        "secure_memory",
    )
    command = [
        os.environ.get("CXX", "c++"),
        "-std=c++20",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-DHID_RUNTIME_NATIVE_TEST",
        "-DHID_CONTROL_EXECUTOR_NATIVE_TEST",
        "-DHID_ROUTE_NATIVE_TEST",
        "-DBLE_LIFECYCLE_NATIVE_TEST",
        "-DBLE_PAIRING_NATIVE_TEST",
    ]
    command.extend(
        f"-I{ROOT / 'firmware/components' / component / 'include'}"
        for component in include_components
    )
    command.append(f"-I{ROOT / 'firmware/components/ble_transport'}")
    command.extend(
        str(path)
        for path in (
            ROOT / "tools/test_hid_control_executor.cpp",
            source,
            ROOT / "firmware/components/hid_runtime/hid_runtime.cpp",
            ROOT / "firmware/components/hid_route/hid_route.cpp",
            ROOT / "firmware/components/usb_lifecycle/usb_lifecycle.cpp",
            ROOT / "firmware/components/ble_lifecycle/ble_lifecycle.cpp",
            ROOT / "firmware/components/ble_pairing/ble_pairing.cpp",
            ROOT / "firmware/components/ble_security/ble_security.cpp",
            ROOT / "firmware/components/secure_memory/secure_memory.cpp",
        )
    )
    command.extend(("-o", str(output)))
    subprocess.run(command, check=True)


def main() -> None:
    original = SOURCE.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for name, (old, new) in MUTATIONS.items():
            if original.count(old) != 1:
                raise RuntimeError(f"mutation anchor drifted: {name}")
            mutated = original.replace(old, new, 1)
            source = temporary / f"{name}.cpp"
            binary = temporary / name
            source.write_text(mutated, encoding="utf-8")
            compile_mutation(source, binary)
            result = subprocess.run(
                (str(binary), "--controller-grace-authority-only"),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            if result.returncode == 0:
                raise AssertionError(f"mutation survived focused tests: {name}")
            print(f"PASS: controller grace tests killed mutation {name}")


if __name__ == "__main__":
    main()
