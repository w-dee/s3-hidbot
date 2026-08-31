#!/usr/bin/env python3
"""Static boundaries for U6.4B2c post-flash provisioning."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / "host/src/hidbot/provisioning_workflow.py"
CLI = ROOT / "host/src/hidbot/cli.py"


def main() -> int:
    workflow = WORKFLOW.read_text(encoding="utf-8")
    cli = CLI.read_text(encoding="utf-8")

    for name, value in {
        "CONTROL_UART_BAUD": "115200",
        "READINESS_DEADLINE_SECONDS": "20.0",
        "MAX_CONNECTION_ATTEMPTS": "4",
        "MAX_DRAIN_SECONDS": "0.5",
        "RX_QUIET_SECONDS": "0.1",
        "MAX_DRAIN_BYTES": "8192",
        "RECONNECT_INTERVAL_SECONDS": "0.25",
        "REQUEST_TIMEOUT_SECONDS": "1.0",
        "CLIENT_MAX_ATTEMPTS": "2",
    }.items():
        assert f"{name} = {value}" in workflow, f"missing fixed B2c policy: {name}"

    assert "execute_flash" in workflow
    assert "compare_firmware_identity" in workflow
    assert "validate_system_info" in workflow
    assert "bundle.artifact_identity" in workflow
    assert "VerificationPhaseClassification.MISMATCH" in workflow
    assert "VerificationPhaseClassification.IDENTITY_UNAVAILABLE" in workflow
    assert "TRANSPORT_UNAVAILABLE" in workflow
    assert "FLASHED_AND_VERIFIED" in workflow
    assert "FLASHED_VERIFICATION_FAILED" in workflow
    assert "FlashPhaseClassification" in workflow
    assert "reset_input_buffer" not in workflow
    for forbidden in (
        "usb_status",
        "release_all",
        "keyboard_report",
        "mouse_report",
        "system.ping",
        "/dev/input",
        "--hardware",
    ):
        assert forbidden not in workflow, f"forbidden post-flash operation: {forbidden}"

    assert "run_post_flash_provisioning" in cli
    assert "_validate_flash_arguments" in cli
    assert "_flash_value" in cli
    assert "_flash_exit_code" in cli
    assert "_print_flash_result" in cli
    assert "VerificationPhaseClassification.MATCH" in cli
    assert "VerificationPhaseClassification.MISMATCH" in cli
    assert "VerificationPhaseClassification.IDENTITY_UNAVAILABLE" in cli
    print("PASS: post-flash provisioning static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
