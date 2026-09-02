#!/usr/bin/env python3
"""Deterministic guards for the deliberately notification-free U7.3 database."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/ble_hid_service/include/ble_hid_service/ble_hid_service.hpp"
SERVICE = ROOT / "firmware/components/ble_hid_service/ble_hid_service.cpp"
TRANSPORT = ROOT / "firmware/components/ble_transport/ble_transport.cpp"
PROJECT_COMPONENTS = ROOT / "firmware/components"


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    service = SERVICE.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")

    report_body = re.search(r"kReportMap\{(.*?)\};", header, re.DOTALL)
    assert report_body is not None
    report_map = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", report_body.group(1)))
    assert len(report_map) == 116
    assert report_map.count(bytes((0x85, 0x01))) == 1  # keyboard report ID
    assert report_map.count(bytes((0x85, 0x02))) == 1  # mouse report ID
    assert bytes((0x95, 0x06, 0x75, 0x08)) in report_map  # six keyboard keys
    assert bytes((0x95, 0x03, 0x81, 0x06)) in report_map  # X/Y/wheel
    assert bytes((0x95, 0x01, 0x81, 0x06)) in report_map  # pan

    assert "kHidInformation{\n    0x11, 0x01, 0x00, 0x00}" in header
    assert "kNeutralKeyboard{}" in header and "uint8_t, 8" in header
    assert "kNeutralMouse{}" in header and "uint8_t, 5" in header
    assert "kKeyboardReportReference{0x01, 0x01}" in header
    assert "kMouseReportReference{0x02, 0x01}" in header

    assert service.count("BLE_UUID16_INIT(0x1812)") == 1
    for uuid in ("0x2a4a", "0x2a4b", "0x2a4c"):
        assert service.count(f"BLE_UUID16_INIT({uuid})") == 1
    assert service.count("BLE_UUID16_INIT(0x2a4d)") == 1
    assert service.count("BLE_UUID16_INIT(0x2908)") == 1
    assert service.count("BLE_GATT_CHR_F_NOTIFY") == 2
    assert service.count("BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY") == 2
    assert "BLE_GATT_CHR_F_WRITE_NO_RSP" in service
    for forbidden in ("0x2a4e", "0x2a22", "0x2a33", "0x180f", "0x180a", "0x2a50"):
        assert forbidden not in service.lower()
    for security_flag in ("BLE_GATT_CHR_F_READ_ENC", "BLE_GATT_CHR_F_WRITE_ENC", "BLE_GATT_CHR_F_READ_AUTHEN", "BLE_GATT_CHR_F_WRITE_AUTHEN"):
        assert security_flag not in service

    project_ble_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in PROJECT_COMPONENTS.glob("ble_*/*.cpp")
    )
    for notification_api in ("ble_gatts_notify", "ble_gatts_indicate"):
        assert notification_api not in project_ble_sources

    assert 'kDeviceName[] = "s3-hidbot"' in transport
    assert "kHidAppearance = 0x03c0" in transport
    assert "kAdvertisingInterval = 64" in transport
    assert "BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP" in transport
    assert "num_uuids16 = 1" in transport and "uuids16_is_complete = 1" in transport
    assert "appearance_is_present = 1" in transport and "name_is_complete = 1" in transport
    assert "ble_gap_adv_rsp_set_fields" not in transport
    # flags (3) + complete UUID (4) + appearance (4) + complete 9-byte name (11)
    assert 3 + 4 + 4 + 11 == 22 <= 31

    print("PASS: BLE HID service/static advertising contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
