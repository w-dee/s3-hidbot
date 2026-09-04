#!/usr/bin/env python3
"""Deterministic guards for the internal BLE HID route and U7.4C retirement."""

from __future__ import annotations

import hashlib
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
CONTROL_PROTOCOL_HEADER = ROOT / "firmware/components/control_protocol/include/control_protocol/control_protocol.hpp"
EXECUTOR = ROOT / "firmware/components/hid_control_executor/hid_control_executor.cpp"
RUNTIME = ROOT / "firmware/components/hid_runtime/hid_runtime.cpp"
RUNTIME_HEADER = ROOT / "firmware/components/hid_runtime/include/hid_runtime/hid_runtime.hpp"
HOST_PROTOCOL = ROOT / "host/src/hidbot/protocol.py"
HOST_CLIENT = ROOT / "host/src/hidbot/client.py"
HOST_CLI = ROOT / "host/src/hidbot/cli.py"
DEPENDENCIES_LOCK = ROOT / "firmware/dependencies.lock"
TINYUSB_HID = ROOT / "firmware/managed_components/espressif__tinyusb/src/class/hid/hid_device.h"


def parse_hid_input_fields(report_map: bytes) -> list[dict[str, int]]:
    """Parse the short-item state attached to every HID Input main item."""

    fields: list[dict[str, int]] = []
    global_state: dict[str, int] = {}
    local_state: dict[str, int] = {}
    global_tags = {
        0: "usage_page",
        1: "logical_minimum",
        2: "logical_maximum",
        7: "report_size",
        8: "report_id",
        9: "report_count",
    }
    local_tags = {1: "usage_minimum", 2: "usage_maximum"}
    offset = 0
    while offset < len(report_map):
        prefix = report_map[offset]
        offset += 1
        assert prefix != 0xFE, "long HID items are not expected in this Report Map"
        size_code = prefix & 0x03
        size = 4 if size_code == 3 else size_code
        assert offset + size <= len(report_map), "truncated HID short item"
        raw = report_map[offset : offset + size]
        offset += size
        value = int.from_bytes(raw, "little", signed=False)
        item_type = (prefix >> 2) & 0x03
        tag = (prefix >> 4) & 0x0F
        if item_type == 1 and tag in global_tags:
            global_state[global_tags[tag]] = value
        elif item_type == 2 and tag in local_tags:
            local_state[local_tags[tag]] = value
        elif item_type == 0:
            if tag == 8:  # Input
                fields.append({**global_state, **local_state, "flags": value})
            local_state.clear()
    return fields


def macro_body(source: str, name: str) -> str:
    match = re.search(rf"#define {re.escape(name)}\(\.\.\.\) \\\n((?:.*\\\n)+)", source)
    assert match is not None
    return match.group(1)


def macro_two_argument_value(body: str, name: str) -> tuple[int, int]:
    match = re.search(
        rf"\b{re.escape(name)}\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)", body
    )
    assert match is not None
    return int(match.group(1)), int(match.group(2))


def integer_constant(source: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(name)}\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;",
        source,
    )
    assert match is not None
    return int(match.group(1), 0)


def byte_array(source: str, name: str) -> bytes:
    match = re.search(rf"\b{re.escape(name)}\{{(.*?)\}};", source, re.DOTALL)
    assert match is not None
    return bytes(
        int(value, 16)
        for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1))
    )


def uuid128_little_endian(source: str, name: str) -> bytes:
    match = re.search(
        rf"ble_uuid128_t\s+{re.escape(name)}\s*=\s*BLE_UUID128_INIT\((.*?)\);",
        source,
        re.DOTALL,
    )
    assert match is not None
    value = bytes(
        int(byte, 16)
        for byte in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1))
    )
    assert len(value) == 16
    return value


def main() -> int:
    header = HEADER.read_text(encoding="utf-8")
    service = SERVICE.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    executor_header = EXECUTOR_HEADER.read_text(encoding="utf-8")
    sdkconfig_defaults = SDKCONFIG_DEFAULTS.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    control_protocol = CONTROL_PROTOCOL.read_text(encoding="utf-8")
    control_protocol_header = CONTROL_PROTOCOL_HEADER.read_text(encoding="utf-8")
    executor = EXECUTOR.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    runtime_header = RUNTIME_HEADER.read_text(encoding="utf-8")
    host_protocol = HOST_PROTOCOL.read_text(encoding="utf-8")
    host_client = HOST_CLIENT.read_text(encoding="utf-8")
    host_cli = HOST_CLI.read_text(encoding="utf-8")
    dependencies_lock = DEPENDENCIES_LOCK.read_text(encoding="utf-8")

    report_body = re.search(r"kReportMap\{(.*?)\};", header, re.DOTALL)
    assert report_body is not None
    report_map = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", report_body.group(1)))
    assert len(report_map) == 116
    assert report_map.count(bytes((0x85, 0x01))) == 1  # keyboard report ID
    assert report_map.count(bytes((0x85, 0x02))) == 1  # mouse report ID
    assert bytes((0x95, 0x06, 0x75, 0x08)) in report_map  # six keyboard keys
    assert bytes((0x95, 0x03, 0x81, 0x06)) in report_map  # X/Y/wheel
    assert bytes((0x95, 0x01, 0x81, 0x06)) in report_map  # pan

    revision_match = re.search(
        r"kGattSchemaRevision\s*=\s*(\d+)", header
    )
    assert revision_match is not None
    schema_revision = int(revision_match.group(1))
    topology_fields = tuple(
        integer_constant(header, name)
        for name in (
            "kGattServiceStartHandle",
            "kLegacyHidServiceStartHandle",
            "kRevision1EpochAttributeCount",
            "kRevision1EpochServiceStartHandle",
            "kRevision1EpochServiceEndHandle",
            "kRevision1HidServiceStartHandle",
            "kRevision1ReportMapValueHandle",
            "kRevision1ControlPointValueHandle",
            "kRevision1KeyboardValueHandle",
            "kRevision1MouseValueHandle",
            "kRevision1HidLastAttributeHandle",
        )
    )
    cache_schema = (
        b"s3-hidbot-cache-schema\x00"
        + bytes((schema_revision,))
        + b"gap-gatt-epoch-hid\x00"
        + uuid128_little_endian(service, "s_schema_epoch_service")
        + uuid128_little_endian(service, "s_schema_epoch_characteristic")
        + b"epoch-primary-read-only-u8\x00"
        + b"".join(value.to_bytes(2, "little") for value in topology_fields)
        + byte_array(header, "kGattSchemaEpochValue")
        + byte_array(header, "kKeyboardReportReference")
        + byte_array(header, "kMouseReportReference")
        + report_map
    )
    schema_fingerprints = {
        1: "980209137b8ab856b406564fadf833be046a386ebe3f8bfed98a044ad2f37cb1",
    }
    assert schema_revision in schema_fingerprints
    cache_schema_fingerprint = hashlib.sha256(cache_schema).hexdigest()
    assert cache_schema_fingerprint == schema_fingerprints[schema_revision], (
        schema_revision,
        cache_schema_fingerprint,
    )

    keyboard_fields = [
        field for field in parse_hid_input_fields(report_map)
        if field.get("report_id") == 1
    ]
    keyboard_array_fields = [
        field for field in keyboard_fields
        if field.get("usage_page") == 0x07
        and field.get("report_count") == 6
        and field.get("report_size") == 8
        and field["flags"] & 0x02 == 0
    ]
    assert len(keyboard_array_fields) == 1
    keyboard_array = keyboard_array_fields[0]
    assert keyboard_array["logical_minimum"] == 0
    assert keyboard_array["logical_maximum"] == 0xFF
    assert keyboard_array["usage_minimum"] == 0
    assert keyboard_array["usage_maximum"] == 0xFF
    assert keyboard_array["usage_minimum"] <= 0x73 <= keyboard_array["usage_maximum"]
    assert keyboard_array["logical_minimum"] <= 0x73 <= keyboard_array["logical_maximum"]
    assert sum(
        field["report_size"] * field["report_count"]
        for field in keyboard_fields
    ) == 8 * 8

    assert "TUD_HID_REPORT_DESC_KEYBOARD()" in main
    tinyusb_dependency = re.search(
        r"  espressif/tinyusb:\n(.*?)(?=\n  [^ \n]+:|\ndirect_dependencies:)",
        dependencies_lock,
        re.DOTALL,
    )
    assert tinyusb_dependency is not None
    # A dependency change must explicitly revalidate the USB descriptor range.
    assert (
        "version: 0b02e68af7a654d5099d8a230291ce19403833ae"
        in tinyusb_dependency.group(1)
    )
    assert (
        "component_hash: 06494427af49510651de7d0935b449a27fdd35ae1cfdd1361a732d4877263223"
        in tinyusb_dependency.group(1)
    )
    assert "git: https://github.com/w-dee/tinyusb.git" in tinyusb_dependency.group(1)
    assert "path: ." in tinyusb_dependency.group(1)
    assert "type: git" in tinyusb_dependency.group(1)
    if TINYUSB_HID.is_file():
        tinyusb_hid = TINYUSB_HID.read_text(encoding="utf-8")
        usb_keyboard = macro_body(tinyusb_hid, "TUD_HID_REPORT_DESC_KEYBOARD")
        assert macro_two_argument_value(usb_keyboard, "HID_LOGICAL_MAX_N") == (255, 2)
        assert macro_two_argument_value(usb_keyboard, "HID_USAGE_MAX_N") == (255, 2)

    firmware_usage_guard = re.search(
        r"bool allowed_keyboard_usage\(.*?\n\}", control_protocol, re.DOTALL
    )
    host_usage_guard = re.search(
        r"def validate_keyboard_report_inputs\(.*?\n    return values",
        host_protocol,
        re.DOTALL,
    )
    assert firmware_usage_guard is not None and host_usage_guard is not None
    firmware_maximum = max(
        int(value, 16)
        for value in re.findall(r"usage <= 0x([0-9a-fA-F]+)U", firmware_usage_guard.group(0))
    )
    host_maximum = max(
        int(value, 16)
        for value in re.findall(r"value <= 0x([0-9a-fA-F]+)", host_usage_guard.group(0))
    )
    assert firmware_maximum == host_maximum
    assert firmware_maximum <= keyboard_array["usage_maximum"]
    assert firmware_maximum <= keyboard_array["logical_maximum"]

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
    report_map_access = re.search(
        r"case AccessTarget::kReportMap: \{(.*?)\n        \}",
        service,
        re.DOTALL,
    )
    assert report_map_access is not None
    report_map_access_body = report_map_access.group(1)
    assert "append(context->om, kReportMap)" in report_map_access_body
    assert "result == 0" in report_map_access_body
    assert "context->op == BLE_GATT_ACCESS_OP_READ_CHR" in report_map_access_body
    assert "BleEventKind::kReportMapRead" in report_map_access_body
    for exact_field in (
        ".generation = s_database->generation_.load(",
        ".connection_handle = connection_handle",
        ".attribute_handle = attribute_handle",
    ):
        assert exact_field in report_map_access_body
    assert report_map_access_body.index("append(context->om, kReportMap)") < (
        report_map_access_body.index("BleEventKind::kReportMapRead")
    )
    assert "value == 0" in service
    assert "OS_MBUF_PKTLEN(context->om) != 1" in service
    assert "length != 1 || value > 1" in service
    assert "BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN" in service
    assert "suspended_.store" not in service

    assert "BLE_GAP_EVENT_SUBSCRIBE" in transport
    for field in ("conn_handle", "attr_handle", "cur_notify", "cur_indicate", "reason"):
        assert f"event->subscribe.{field}" in transport
    for reason in ("WRITE", "TERM", "RESTORE"):
        assert f"BLE_GAP_SUBSCRIBE_REASON_{reason}" in transport
    assert "database_->on_subscribe" not in transport
    assert "BleEventKind::kSubscription" in transport
    assert re.search(
        r"BleEventKind::\s*kServiceChangedSubscription", transport
    )
    assert "BLE_UUID16_INIT(0x1801)" in transport
    assert "BLE_UUID16_INIT(0x2a05)" in transport
    assert re.search(
        r"ble_gatts_find_chr\(\s*&s_gatt_service_uuid\.u", transport
    )
    assert transport.count("ble_svc_gatt_changed(start_handle, end_handle)") == 1
    assert "BLE_GAP_EVENT_NOTIFY_TX" not in transport
    assert "kGattChangedStartHandle = 0x0001" in executor_header
    assert "kGattChangedEndHandle = 0xffff" in executor_header
    assert "kSchemaNamespace[] = \"hid_schema\"" in transport
    assert "nvs_set_u8(handle, key," in transport
    assert "nvs_commit(handle)" in transport
    assert "read_schema_revision(identity, verified)" in transport
    assert "result == ESP_ERR_NVS_NOT_FOUND" in transport
    schema_status = transport[
        transport.index("Backend::gatt_schema_status(") :
        transport.index("Backend::persist_gatt_schema_current(")
    ]
    missing_revision = re.search(
        r"if \(result == ESP_ERR_NVS_NOT_FOUND\) \{(.*?)\n    \}",
        schema_status,
        re.DOTALL,
    )
    assert missing_revision is not None and "Kind::kStale" in missing_revision.group(1)
    assert "descriptor.peer_id_addr" in transport
    assert "identity.type" in transport and "identity.val[index]" in transport
    assert transport.count("nvs_erase_key(handle, key)") == 1
    store_delete_start = transport.index("int Backend::store_delete(")
    store_delete_end = transport.index("int Backend::store_status(")
    store_delete = transport[store_delete_start:store_delete_end]
    for token in (
        "original_store_delete_(object_type, key)",
        "object_type == BLE_STORE_OBJ_TYPE_OUR_SEC",
        "object_type == BLE_STORE_OBJ_TYPE_PEER_SEC",
        "valid_identity(key->sec.peer_addr)",
        "delete_schema_revision_verified(key->sec.peer_addr)",
        "StoreFailureKind::kDelete",
        "return BLE_HS_ESTORE_FAIL",
    ):
        assert token in store_delete
    assert store_delete.index(
        "delete_schema_revision_verified(key->sec.peer_addr)") < store_delete.index(
            "original_store_delete_(object_type, key)")
    assert "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1" in sdkconfig_defaults
    orphan = re.search(
        r"Backend::terminate_orphan_connection\((.*?)\n\}", transport, re.DOTALL
    )
    assert orphan is not None
    assert "ble_gap_terminate(connection_handle" in orphan.group(1)
    signal = re.search(
        r"bool Controller::signal_ble_event\(BleEvent event\) \{(.*?)\n\}",
        (ROOT / "firmware/components/hid_control_executor/hid_control_executor.cpp").read_text(encoding="utf-8"),
        re.DOTALL,
    )
    assert signal is not None
    assert "event.kind == BleEventKind::kConnect" in signal.group(1)
    assert "terminate_orphan_connection(" in signal.group(1)
    assert "submit_ble_keyboard" not in main
    assert "submit_ble_mouse" not in main
    public_sources = "\n".join(
        (control_protocol, control_protocol_header, host_protocol, host_client, host_cli)
    )
    assert "hid.output-route-v2" in public_sources
    assert "hid.route.v2.status" in public_sources
    assert "hid.route.v2.set" in public_sources
    assert "hid.route.set route must be none or usb" in control_protocol
    assert 'value == "none"' in control_protocol
    assert 'value == "usb"' in control_protocol
    assert 'allow_ble && value == "ble"' in control_protocol
    output_route = re.search(
        r"enum class OutputRoute.*?\{(.*?)\};", control_protocol_header, re.DOTALL
    )
    assert output_route is not None and "kBle" in output_route.group(1)
    host_route = re.search(r"class OutputRoute.*?\n\n", host_protocol, re.DOTALL)
    assert host_route is not None and 'BLE = "ble"' not in host_route.group(0)
    host_route_v2 = re.search(r"class OutputRouteV2.*?\n\n", host_protocol, re.DOTALL)
    assert host_route_v2 is not None and 'BLE = "ble"' in host_route_v2.group(0)

    assert "activate_ble_route_internal" in executor
    assert "request_route_ble" in executor
    assert "desired == hid_route::OutputRoute::kBle" in executor
    assert "ActionKind::kRouteBleActivate" in executor
    assert "route_rpc_result_ = activate_ble_route()" in executor
    assert "ble_route_ready()" in executor
    assert "ActionKind::kBleHidReport" in executor
    assert "process_ble_report" in executor
    assert "submit_runtime_ble_report" in executor
    assert "retire_ble_route_if_unready" in executor
    assert "ble_work_token_current" in executor
    assert "BleSubmitResult::kStackAccepted" in runtime
    assert "BleSubmitResult::kResourceFailure" in runtime
    assert "BleSubmitResult::kStackRejected" in runtime
    assert "retire_ble_route_if_matches" in runtime
    assert "ble_action_pending" in runtime_header
    assert "mark_ble_report_scheduled" in runtime
    assert "abandon_ble_report" in runtime
    assert "bind_authority_event_sink" in executor
    assert "signal_hid_authority_change" in executor

    # U7.4C: a safety-only old-route tuple survives normal-authority revoke,
    # receives one exact all-up attempt per interface, then a dedicated
    # nonblocking 100 ms timer drives the already-hardened disconnect path.
    assert "kBleRouteReleaseGraceMs = 100" in executor_header
    assert "kBleKeyboardAllUp{}" in executor_header
    assert "kBleMouseAllUp{}" in executor_header
    assert "begin_ble_release" in runtime
    assert "complete_ble_route_release_if_matches" in runtime
    assert "ble_route_releasing_" in runtime_header
    assert "ble_route_release_epoch_" in runtime_header
    release_submit = re.search(
        r"void Controller::submit_ble_safety_release\((.*?)\n\}",
        executor,
        re.DOTALL,
    )
    assert release_submit is not None
    assert release_submit.group(1).count("notify_custom(") == 2
    assert "kBleKeyboardAllUp.data()" in release_submit.group(1)
    assert "kBleMouseAllUp.data()" in release_submit.group(1)
    assert "vTaskDelay" not in release_submit.group(1)
    assert "arm_ble_route_release_grace(identity)" in executor
    assert "ble_route_grace_due_.store(true" in executor
    assert "request_executor_wake();" in executor
    assert "route_release_timer_" in transport
    assert 'name = "ble_route_release"' in transport
    assert "&route_release_timer_" in transport
    assert "&timeout_timer_" in transport
    assert (
        "hid_control_executor::kBleRouteReleaseGraceMs) * 1000U" in transport
    )
    assert "start_ble_route_disconnect" in executor
    assert "ble_backend_->disconnect(identity.connection_handle)" in executor
    assert "complete_ble_route_release_on_disconnect" in executor
    assert "ble_route_disconnect_observed_.store(true" in executor
    disable = re.search(
        r"BleCommandOutcome Controller::request_ble_disable\(\) \{(.*?)\n\}",
        executor,
        re.DOTALL,
    )
    assert disable is not None
    assert "route.active == hid_route::OutputRoute::kBle" in disable.group(1)
    assert "route.active != hid_route::OutputRoute::kNone" not in disable.group(1)
    assert "route.transition != hid_route::Transition::kStable" not in disable.group(1)

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
