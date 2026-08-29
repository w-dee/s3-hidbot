#!/usr/bin/env python3
"""Static checks for the U1 default firmware HID-safety contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main/blink.cpp"
KCONFIG = ROOT / "firmware/main/Kconfig.projbuild"


def main() -> int:
    source = MAIN.read_text(encoding="utf-8")
    kconfig = KCONFIG.read_text(encoding="utf-8")
    guard = "#if defined(CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC) && CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC"

    assert "default n" in kconfig
    assert source.count(guard) == 5
    assert source.count("#endif") >= source.count(guard)
    assert "tud_hid_n_keyboard_report" not in source
    assert "queue_mouse_report" in source
    assert "tud_hid_n_mouse_report" not in source
    assert "GPIO_NUM_19" not in source
    assert "GPIO_NUM_20" not in source
    print("PASS: default BOOT diagnostic and HID safety static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
