#!/usr/bin/env python3
"""Static GATT topology and persisted-CCCD migration guards for U7.4."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/components/ble_hid_service/include/ble_hid_service/ble_hid_service.hpp"
SERVICE = ROOT / "firmware/components/ble_hid_service/ble_hid_service.cpp"
TRANSPORT = ROOT / "firmware/components/ble_transport/ble_transport.cpp"
SDKCONFIG_DEFAULTS = ROOT / "firmware/sdkconfig.defaults"


def integer_constant(source: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(name)}\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;",
        source,
    )
    assert match is not None, name
    return int(match.group(1), 0)


def uuid128_little_endian(source: str, name: str) -> bytes:
    match = re.search(
        rf"ble_uuid128_t\s+{re.escape(name)}\s*=\s*BLE_UUID128_INIT\((.*?)\);",
        source,
        re.DOTALL,
    )
    assert match is not None, name
    value = bytes(
        int(byte, 16)
        for byte in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1))
    )
    assert len(value) == 16
    return value


def canonical_uuid(little_endian: bytes) -> str:
    value = little_endian[::-1].hex()
    return f"{value[:8]}-{value[8:12]}-{value[12:16]}-{value[16:20]}-{value[20:]}"


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    service = SERVICE.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    sdkconfig = SDKCONFIG_DEFAULTS.read_text(encoding="utf-8")

    service_uuid = uuid128_little_endian(service, "s_schema_epoch_service")
    characteristic_uuid = uuid128_little_endian(
        service, "s_schema_epoch_characteristic"
    )
    assert canonical_uuid(service_uuid) == "5f7d0a10-7e38-4ed1-b97b-1fa4e83c2a10"
    assert canonical_uuid(characteristic_uuid) == (
        "5f7d0a11-7e38-4ed1-b97b-1fa4e83c2a10"
    )
    assert service_uuid != characteristic_uuid

    epoch_characteristics = re.search(
        r"ble_gatt_chr_def s_schema_epoch_characteristics\[\]\s*=\s*\{(.*?)\n\};",
        service,
        re.DOTALL,
    )
    assert epoch_characteristics is not None
    epoch_body = epoch_characteristics.group(1)
    assert epoch_body.count(".uuid =") == 1
    assert epoch_body.count(".uuid = &s_schema_epoch_characteristic.u") == 1
    assert epoch_body.count(".descriptors = nullptr") == 1
    assert epoch_body.count("BLE_GATT_CHR_F_READ") == 1
    for forbidden in (
        "BLE_GATT_CHR_F_WRITE",
        "BLE_GATT_CHR_F_NOTIFY",
        "BLE_GATT_CHR_F_INDICATE",
    ):
        assert forbidden not in epoch_body
    assert ".val_handle = &s_schema_epoch_value_handle" in epoch_body
    assert epoch_body.count(".access_cb = Database::access") == 1
    epoch_access = re.search(
        r"case AccessTarget::kSchemaEpoch:(.*?)case AccessTarget::kInformation:",
        service,
        re.DOTALL,
    )
    assert epoch_access is not None
    assert "append(context->om, kGattSchemaEpochValue)" in epoch_access.group(1)
    assert "signal_ble_event" not in epoch_access.group(1)

    services = re.search(
        r"ble_gatt_svc_def s_services\[\]\s*=\s*\{(.*?)\n\};",
        service,
        re.DOTALL,
    )
    assert services is not None
    services_body = services.group(1)
    epoch_index = services_body.index(".uuid = &s_schema_epoch_service.u")
    hid_index = services_body.index(".uuid = &s_hid_service.u")
    assert epoch_index < hid_index
    assert services_body.count("BLE_GATT_SVC_TYPE_PRIMARY") == 2
    assert services_body.count(".uuid =") == 2
    assert services_body[hid_index:].count(".uuid =") == 1
    assert (
        services_body.count(".characteristics = s_schema_epoch_characteristics")
        == 1
    )
    assert services_body.count(".characteristics = s_characteristics") == 1
    hid_characteristics = re.search(
        r"ble_gatt_chr_def s_characteristics\[\]\s*=\s*\{(.*?)\n\};",
        service,
        re.DOTALL,
    )
    assert hid_characteristics is not None
    hid_body = hid_characteristics.group(1)
    assert hid_body.count(".uuid =") == 5
    assert ".uuid = &s_report_map.u" in hid_body
    assert hid_body.count(".uuid = &s_report.u") == 2
    assert ".descriptors = s_keyboard_descriptors" in hid_body
    assert ".descriptors = s_mouse_descriptors" in hid_body
    for hid_only in ("s_report_map.u", "s_report.u"):
        assert hid_only not in epoch_body

    # ESP-IDF v5.5.4 sequentially consumes queued service-definition arrays.
    # With the locked configuration, GAP consumes five attributes and GATT
    # eight.  The revision-1 epoch then consumes three before the 15-attribute
    # HID service.  The last service's discovery tuple ends at 0xffff.
    assert "# CONFIG_BT_NIMBLE_GATT_CACHING is not set" in sdkconfig
    gap_start = 0x0001
    gap_attributes = 5
    gatt_start = gap_start + gap_attributes
    gatt_attributes = 8
    epoch_start = gatt_start + gatt_attributes
    epoch_attributes = integer_constant(
        header, "kRevision1EpochAttributeCount"
    )
    assert epoch_attributes == 1 + 2 * epoch_body.count(".uuid =") == 3
    hid_start = epoch_start + epoch_attributes
    hid_attributes = 15
    expected = {
        "kGattServiceStartHandle": gatt_start,
        "kLegacyHidServiceStartHandle": epoch_start,
        "kLegacyReportMapValueHandle": epoch_start + 4,
        "kLegacyKeyboardValueHandle": epoch_start + 8,
        "kLegacyMouseValueHandle": epoch_start + 12,
        "kRevision1EpochServiceStartHandle": epoch_start,
        "kRevision1EpochServiceEndHandle": hid_start - 1,
        "kRevision1HidServiceStartHandle": hid_start,
        "kRevision1ReportMapValueHandle": hid_start + 4,
        "kRevision1ControlPointValueHandle": hid_start + 6,
        "kRevision1KeyboardValueHandle": hid_start + 8,
        "kRevision1MouseValueHandle": hid_start + 12,
        "kRevision1HidLastAttributeHandle": hid_start + hid_attributes - 1,
    }
    for name, value in expected.items():
        assert integer_constant(header, name) == value, (name, value)
    assert integer_constant(header, "kGattSchemaRevision") == 1
    assert epoch_start == 0x000E and hid_start == 0x0011
    assert "ble_svc_gap_init();" in transport
    assert "ble_svc_gatt_init();" in transport
    assert "database->register_database()" in transport
    assert (
        transport.index("ble_svc_gap_init();")
        < transport.index("ble_svc_gatt_init();")
        < transport.index("database->register_database()")
        < transport.index("nimble_port_freertos_init(host_task)")
    )
    component_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (ROOT / "firmware/components").glob("*/*.cpp")
    )
    assert component_sources.count("ble_gatts_count_cfg(") == 1
    assert component_sources.count("ble_gatts_add_svcs(") == 1
    assert "ble_gatts_find_svc(&s_gatt_service.u" in service
    assert "ble_gatts_find_svc(&s_schema_epoch_service.u" in service
    assert "ble_gatts_find_svc(&s_hid_service.u" in service
    for name in expected:
        if name.startswith("kRevision1") and name != "kRevision1EpochAttributeCount":
            assert name in service or name == "kRevision1HidLastAttributeHandle"

    # NimBLE stores CCCDs by peer identity and characteristic value handle.
    # Its v5.5.4 restore path ignores old handles for live client-config state
    # but still reports RESTORE and retains the record.  Project callbacks and
    # the executor accept subscription evidence only for registered current
    # value handles, and the fixed store capacity covers all three migrated
    # bonds without deleting them.
    legacy_hid = {
        expected["kLegacyKeyboardValueHandle"],
        expected["kLegacyMouseValueHandle"],
    }
    current_notifiable = {
        0x0008,  # stable Service Changed value handle
        expected["kRevision1KeyboardValueHandle"],
        expected["kRevision1MouseValueHandle"],
    }
    assert legacy_hid.isdisjoint(current_notifiable)
    assert "event->subscribe.attr_handle == handles.keyboard_value" in transport
    assert "event->subscribe.attr_handle == handles.mouse_value" in transport
    assert (
        "interface != hid_control_executor::BleHidInterface::kUnknown"
        in transport
    )
    max_bonds_match = re.search(
        r"CONFIG_BT_NIMBLE_MAX_BONDS=(\d+)", sdkconfig
    )
    max_cccds_match = re.search(
        r"CONFIG_BT_NIMBLE_MAX_CCCDS=(\d+)", sdkconfig
    )
    assert max_bonds_match is not None
    assert max_cccds_match is not None
    max_bonds = int(max_bonds_match.group(1))
    max_cccds = int(max_cccds_match.group(1))
    assert max_bonds == 3
    assert max_cccds == max_bonds * (1 + 2 + 2) == 15

    print(
        "PASS: BLE HID topology epoch "
        "(GATT 0x0006-0x000d, epoch 0x000e-0x0010, HID 0x0011-0xffff)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
