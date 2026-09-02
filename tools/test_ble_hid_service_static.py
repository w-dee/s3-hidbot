#!/usr/bin/env python3
"""Deterministic guards for the internal-only U7.4A BLE HID foundation."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/ble_hid_service/include/ble_hid_service/ble_hid_service.hpp"
SERVICE = ROOT / "firmware/components/ble_hid_service/ble_hid_service.cpp"
TRANSPORT = ROOT / "firmware/components/ble_transport/ble_transport.cpp"
EXECUTOR_HEADER = ROOT / "firmware/components/hid_control_executor/include/hid_control_executor/hid_control_executor.hpp"
SDKCONFIG_DEFAULTS = ROOT / "firmware/sdkconfig.defaults"
PROJECT_COMPONENTS = ROOT / "firmware/components"
MAIN = ROOT / "firmware/main/main.cpp"
CONTROL_PROTOCOL = ROOT / "firmware/components/control_protocol/control_protocol.cpp"


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    service = SERVICE.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    executor_header = EXECUTOR_HEADER.read_text(encoding="utf-8")
    sdkconfig_defaults = SDKCONFIG_DEFAULTS.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    control_protocol = CONTROL_PROTOCOL.read_text(encoding="utf-8")

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
    assert service.count("BLE_GATT_CHR_F_NOTIFY") == 6
    assert service.count("BLE_GATT_CHR_F_READ_AUTHEN") == 2
    assert service.count("BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN") == 2
    assert service.count("BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR") == 2
    assert "BLE_GATT_CHR_F_WRITE_NO_RSP" in service
    for forbidden in ("0x2a4e", "0x2a22", "0x2a33", "0x180f", "0x180a", "0x2a50"):
        assert forbidden not in service.lower()
    assert service.count("BLE_GATT_CHR_F_WRITE_AUTHEN") == 1

    assert "validate_registered_database() override" in header
    assert "virtual int validate_registered_database() = 0" in executor_header
    assert "ble_gatts_find_svc(&s_hid_service.u" in service
    for uuid in ("s_hid_information.u", "s_report_map.u", "s_control_point.u"):
        assert f"{{&{uuid}," in service
    for handle in (
        "s_information_value_handle",
        "s_report_map_value_handle",
        "s_control_point_value_handle",
        "s_keyboard_value_handle",
        "s_mouse_value_handle",
    ):
        assert f"&{handle}" in service
    assert "s_keyboard_value_handle == 0" in service
    assert "s_mouse_value_handle == 0" in service

    project_ble_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in PROJECT_COMPONENTS.glob("ble_*/*.cpp")
    )
    assert len(re.findall(r"\bble_gatts_notify_custom\s*\(", project_ble_sources)) == 1
    assert not re.search(r"\bble_gatts_notify\s*\(", project_ble_sources)
    assert not re.search(r"\bble_gatts_indicate(?:_custom)?\s*\(", project_ble_sources)
    assert "ble_hs_mbuf_from_flat(payload, payload_length)" in service
    assert "BleNotifyBackendResult::kResourceFailure" in service
    assert "BleNotifyBackendResult::kStackAccepted" in service
    assert "bind_event_sink" in header
    assert "BleEventKind::kControlPoint" in service
    assert "value == 0" in service
    assert "OS_MBUF_PKTLEN(context->om) != 1" in service
    assert "length != 1 || value > 1" in service
    assert "BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN" in service
    assert "suspended_.store" not in service

    assert "BLE_GAP_EVENT_SUBSCRIBE" in transport
    for field in ("conn_handle", "attr_handle", "cur_notify", "reason"):
        assert f"event->subscribe.{field}" in transport
    for reason in ("WRITE", "TERM", "RESTORE"):
        assert f"BLE_GAP_SUBSCRIBE_REASON_{reason}" in transport
    assert "database_->on_subscribe" not in transport
    assert "BleEventKind::kSubscription" in transport
    assert "submit_ble_keyboard" not in main
    assert "submit_ble_mouse" not in main
    assert "hid.output-route-v2" not in control_protocol
    assert "hid.route.v2." not in control_protocol
    assert "hid.route.set route must be none or usb" in control_protocol

    assert 'kDeviceName[] = "s3-hidbot"' in transport
    assert "kHidAppearance = 0x03c0" in transport
    assert "kAdvertisingInterval = 64" in transport
    assert "BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP" in transport
    assert "num_uuids16 = 1" in transport and "uuids16_is_complete = 1" in transport
    assert "appearance_is_present = 1" in transport and "name_is_complete = 1" in transport
    assert "ble_gap_adv_rsp_set_fields" not in transport
    standard_gap = transport.index("ble_svc_gap_init();")
    standard_gatt = transport.index("ble_svc_gatt_init();")
    project_registration = transport.index("database->register_database()")
    host_start = transport.index("nimble_port_freertos_init(host_task)")
    assert standard_gap < standard_gatt < project_registration < host_start
    assert standard_gatt < transport.index("ble_svc_gap_device_name_set(kDeviceName)")
    assert standard_gatt < transport.index("ble_svc_gap_device_appearance_set(kHidAppearance)")
    validation = transport.index("database_->validate_registered_database()")
    advertising_fields = transport.index("ble_hs_adv_fields fields{}")
    advertising_start = transport.index("ble_gap_adv_start(")
    assert host_start < validation < advertising_fields < advertising_start
    assert 'CONFIG_BT_NIMBLE_GAP_SERVICE=y' in sdkconfig_defaults
    assert 'CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME="s3-hidbot"' in sdkconfig_defaults
    assert "CONFIG_BT_NIMBLE_SVC_GAP_APPEARANCE=960" in sdkconfig_defaults
    # flags (3) + complete UUID (4) + appearance (4) + complete 9-byte name (11)
    assert 3 + 4 + 4 + 11 == 22 <= 31

    print("PASS: BLE HID service/static advertising contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
