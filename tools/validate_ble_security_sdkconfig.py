#!/usr/bin/env python3
"""Validate effective U7.5A ESP-IDF configuration after generation."""
from pathlib import Path
import sys

EXPECTED = {
    "CONFIG_BT_NIMBLE_SECURITY_ENABLE": "y",
    "CONFIG_BT_NIMBLE_SM_LEGACY": "y",
    "CONFIG_BT_NIMBLE_SM_SC": "y",
    "CONFIG_BT_NIMBLE_SM_LVL": "3",
    "CONFIG_BT_NIMBLE_SM_SC_ONLY": "0",
    "CONFIG_BT_NIMBLE_NVS_PERSIST": "y",
    "CONFIG_BT_NIMBLE_MAX_BONDS": "3",
    "CONFIG_BT_NIMBLE_MAX_CCCDS": "15",
}

def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_ble_security_sdkconfig.py SDKCONFIG")
    text = Path(sys.argv[1]).read_text(encoding="utf-8")
    values = dict(line.split("=", 1) for line in text.splitlines()
                  if line.startswith("CONFIG_") and "=" in line)
    for key, expected in EXPECTED.items():
        assert values.get(key) == expected, f"{key}: expected {expected}, got {values.get(key)}"
    disabled = "CONFIG_BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION"
    assert disabled not in values
    assert f"# {disabled} is not set" in text
    print("PASS: effective BLE security sdkconfig")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
