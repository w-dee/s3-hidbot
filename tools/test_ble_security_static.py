#!/usr/bin/env python3
"""Static guards for the private U7.5A security prerequisite."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    transport = (ROOT / "firmware/components/ble_transport/ble_transport.cpp").read_text()
    service = (ROOT / "firmware/components/ble_hid_service/ble_hid_service.cpp").read_text()
    defaults = (ROOT / "firmware/sdkconfig.defaults").read_text()
    protocol = (ROOT / "firmware/components/control_protocol/control_protocol.cpp").read_text()
    for assignment in (
        "ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;",
        "ble_hs_cfg.sm_bonding = 1;", "ble_hs_cfg.sm_mitm = 1;",
        "ble_hs_cfg.sm_sc = 1;", "ble_hs_cfg.sm_sc_only = 0;",
        "ble_hs_cfg.sm_sec_lvl = 3;",
        "ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;",
    ):
        assert transport.count(assignment) == 1
    assert "BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID" in transport
    assert "BLE_SM_PAIR_KEY_DIST_SIGN" not in transport
    assert "BLE_SM_PAIR_KEY_DIST_LINK" not in transport
    for value in (
        "CONFIG_BT_NIMBLE_SECURITY_ENABLE=y", "CONFIG_BT_NIMBLE_SM_LEGACY=y",
        "CONFIG_BT_NIMBLE_SM_SC=y", "CONFIG_BT_NIMBLE_SM_LVL=3",
        "CONFIG_BT_NIMBLE_SM_SC_ONLY=0", "CONFIG_BT_NIMBLE_NVS_PERSIST=y",
        "CONFIG_BT_NIMBLE_MAX_BONDS=3",
        "# CONFIG_BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION is not set",
    ):
        assert defaults.count(value) == 1
    assert transport.index("ble_store_config_init();") < transport.index(
        "original_store_read_ = ble_hs_cfg.store_read_cb;") < transport.index(
        "ble_hs_cfg.store_read_cb = store_read;")
    assert "ble_store_util_status_rr" not in transport
    assert "return BLE_HS_ESTORE_CAP;" in transport
    assert transport.count("original_store_write_(object_type, value)") == 1
    assert transport.count("original_store_delete_(object_type, key)") == 1
    assert "ble_store_read_our_sec(&key, &our)" in transport
    assert "ble_store_read_peer_sec(&key, &peer)" in transport

    control = re.search(r"kControlPoint\),(.*?)\.val_handle", service, re.S)
    assert control and "BLE_GATT_CHR_F_WRITE_NO_RSP" in control.group(1)
    assert "BLE_GATT_CHR_F_WRITE_AUTHEN" in control.group(1)
    assert ".min_key_size = 16" in control.group(1)
    for report in ("kKeyboardReport", "kMouseReport"):
        body = re.search(rf"{report}\),(.*?)\.val_handle", service, re.S)
        assert body
        for flag in ("BLE_GATT_CHR_F_READ_AUTHEN", "BLE_GATT_CHR_F_NOTIFY",
                     "BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN",
                     "BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR"):
            assert flag in body.group(1)
        assert ".min_key_size = 16" in body.group(1)
    assert service.count(".att_flags = BLE_ATT_F_READ") == 2
    assert service.count(".min_key_size = 0") == 2
    for forbidden in ("ble.pairing.status", "ble.pairing.respond",
                      "ble.pairing-transaction-v1", "ble.pairing-control-v1",
                      "ble.bond-store-v1"):
        assert forbidden not in protocol

    sources = "\n".join(path.read_text() for path in
        (ROOT / "firmware/components").glob("ble_*/*.cpp"))
    for call in re.findall(r"ESP_LOG[A-Z]+\([^;]+;", sources):
        assert not re.search(r"(ltk|irk|ediv|rand_num|passkey|peer.*addr)", call, re.I)
    print("PASS: BLE security static contract")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
