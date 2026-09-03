#!/usr/bin/env python3
"""Static U7.5B guards for exact, fail-closed bond administration."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str, next_signature: str) -> str:
    match = re.search(
        re.escape(signature) + r"(.*?)" + re.escape(next_signature), source, re.S
    )
    assert match, signature
    return match.group(1)


def main() -> int:
    transport = (ROOT / "firmware/components/ble_transport/ble_transport.cpp").read_text()
    recovery = (ROOT / "firmware/components/ble_transport/persistent_store_recovery.hpp").read_text()
    executor = (ROOT / "firmware/components/hid_control_executor/hid_control_executor.cpp").read_text()
    protocol = (ROOT / "firmware/components/control_protocol/control_protocol.cpp").read_text()
    protocol_header = (ROOT / "firmware/components/control_protocol/include/control_protocol/control_protocol.hpp").read_text()
    main_cpp = (ROOT / "firmware/main/main.cpp").read_text()
    host_protocol = (ROOT / "host/src/hidbot/protocol.py").read_text()
    host_client = (ROOT / "host/src/hidbot/client.py").read_text()
    host_cli = (ROOT / "host/src/hidbot/cli.py").read_text()

    for value in (
        "ble.bond-administration-v1",
        "ble.bond.list",
        "ble.bond.remove",
        "BLE_BOND_NOT_FOUND",
        "BLE_BOND_AMBIGUOUS",
        "BLE_BOND_BUSY",
        "BLE_BOND_STORAGE",
    ):
        assert value in protocol
    assert protocol.count("ble.bond-administration-v1") == 2
    assert "kBondIdHexChars = 32" in protocol_header

    identifier = function(transport, "bool make_bond_id(", "void schema_key(")
    assert 'kBondIdDomain[] = "s3-hidbot/bond-id/v1"' in transport
    assert "MBEDTLS_MD_SHA256" in identifier
    assert "kBondIdHexChars / 2" in identifier
    assert "identity.type" in identifier and "identity.val" in identifier

    listing = function(
        transport,
        "hid_control_executor::BleBondListResult Backend::list_bonds() {",
        "hid_control_executor::BleBondRemoveResult Backend::remove_bond(",
    )
    for required in (
        "ble_store_read_our_sec",
        "ble_store_read_peer_sec",
        "persisted_bond_is_valid",
        "read_schema_revision",
        "std::strcmp",
        "result.healthy = false",
        "kBondCapacity + 1",
    ):
        assert required in listing
    for secret_field in ("ltk", "irk", "csrk"):
        assert not re.search(rf'"{secret_field}"', protocol, re.I)
    assert "peer_addr" not in protocol

    removal = function(
        transport,
        "hid_control_executor::BleBondRemoveResult Backend::remove_bond(",
        "void Backend::refresh_security(",
    )
    for required in (
        "const auto before = list_bonds()",
        "match_count != 1",
        "!before.healthy",
        ".connected",
        "ble_store_util_bonded_peers",
        "exact_identity_matches != 1",
        "run_schema_first_removal",
        "delete_schema_revision_verified(target)",
        "ble_store_util_delete_peer(&target)",
        "ble_store_read_our_sec",
        "ble_store_read_peer_sec",
        "read_schema_revision(target",
        "verify_peer_auxiliary_absent(target)",
        "after = list_bonds()",
        "after.count + 1U == before.count",
        "others_preserved",
        "new_bond.schema_revision",
        "new_bond.schema_current",
        "StoreFailureKind::kDelete",
    ):
        assert required in removal
    for forbidden in ("delete_all_bonds", "remove_oldest", "round_robin"):
        assert forbidden not in removal.lower()

    store_delete = function(
        transport, "int Backend::store_delete(", "int Backend::store_status("
    )
    assert "classify_store_delete_callback_result(result" in store_delete
    assert "BLE_HS_ENOENT" in store_delete
    assert "StoreDeleteCallbackResult::kFailure" in store_delete
    assert "if (result != 0)" not in store_delete
    assert "original_store_delete_(object_type, key)" in store_delete
    assert "delete_schema_revision_verified(key->sec.peer_addr)" in store_delete
    assert store_delete.index(
        "delete_schema_revision_verified(key->sec.peer_addr)") < store_delete.index(
            "original_store_delete_(object_type, key)")
    assert "BLE_STORE_OBJ_TYPE_OUR_SEC" in store_delete
    assert "BLE_STORE_OBJ_TYPE_PEER_SEC" in store_delete

    initialization = function(
        transport, "std::int32_t Backend::initialize(",
        "void Backend::set_generation(",
    )
    for required in (
        "ble_store_config_init()",
        "ble_hs_cfg.store_delete_cb = store_delete",
        "recover_orphan_schema_records()",
        "ble_svc_gap_init()",
        "nimble_port_freertos_init(host_task)",
    ):
        assert required in initialization
    assert initialization.index("ble_store_config_init()") < initialization.index(
        "recover_orphan_schema_records()")
    assert initialization.index(
        "ble_hs_cfg.store_delete_cb = store_delete") < initialization.index(
            "recover_orphan_schema_records()")
    assert initialization.index("recover_orphan_schema_records()") < initialization.index(
        "ble_svc_gap_init()")
    assert initialization.index("recover_orphan_schema_records()") < initialization.index(
        "nimble_port_freertos_init(host_task)")

    for required in (
        "ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 4)",
        "MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST) == 1",
        "MYNEWT_VAL(BLE_STORE_MAX_CCCDS) == 15",
        "MYNEWT_VAL(ENC_ADV_DATA) == 0",
        "MYNEWT_VAL(BLE_HOST_BASED_PRIVACY) == 0",
        'kNimbleStoreNamespace[] = "nimble_bond"',
        '"our_sec_1", "our_sec_2", "our_sec_3"',
        '"peer_sec_1", "peer_sec_2", "peer_sec_3"',
        "NVS_TYPE_BLOB",
        "sizeof(ble_store_value_sec)",
        "read_persistent_security",
        "read_restored_security",
        "read_schema_identities",
        "make_recovery_plan",
        "delete_schema_revision_verified",
    ):
        assert required in transport
    for required in (
        "kSchemaRecoveryCapacity = 16",
        "same_set",
        "A half bond is globally inconsistent",
        "make_recovery_plan",
        "run_orphan_recovery",
        "run_schema_first_removal",
        "kSchemaDelete",
        "kPeerDelete",
        "kPostcondition",
    ):
        assert required in recovery

    eligible = function(
        executor,
        "bool Controller::bond_remove_eligible() const {",
        "ble_pairing::RespondResult Controller::respond_to_pairing(",
    )
    for required in (
        "DesiredExposure::kHidden",
        "ObservedState::kIdle",
        "!lifecycle.advertising",
        "!lifecycle.connected",
        "!lifecycle.recovery_required",
        "!route.invalidation_pending",
        "route.desired != hid_route::OutputRoute::kBle",
        "route.active != hid_route::OutputRoute::kBle",
        "route.transition == hid_route::Transition::kStable",
        "pairing.coherent",
        "LiveState::kIdle",
        "!pairing.pairing_active",
    ):
        assert required in eligible

    process_remove = re.search(
        r"if \(action.kind == ActionKind::kBondRemove\) \{(.*?)\n    \}",
        executor,
        re.S,
    )
    assert process_remove
    assert process_remove.group(1).index("bond_remove_eligible()") < process_remove.group(1).index("remove_bond(bond_id)")
    assert "persistent_store_failure_observed()" in process_remove.group(1)
    assert process_remove.group(1).index(
        "persistent_store_failure_observed()") < process_remove.group(1).index(
            "!lifecycle.stack_ready")
    assert "lifecycle.recovery_required" not in process_remove.group(1)

    process_list = re.search(
        r"if \(action.kind == ActionKind::kBondList\) \{(.*?)\n    \}",
        executor,
        re.S,
    )
    assert process_list
    assert process_list.group(1).index(
        "persistent_store_failure_observed()") < process_list.group(1).index(
            "lifecycle.recovery_required")
    assert "BleBondListResultKind::kStorageFailure" in process_list.group(1)
    assert "ControlOperation::kBondAdministration" in executor
    assert "ble_bond_list_provider" in main_cpp
    assert "ble_bond_remove_provider" in main_cpp
    assert "source.bonds[left].bond_id == source.bonds[right].bond_id" in main_cpp
    assert "BleBondListResultKind::kStorageFailure" in main_cpp

    for required in (
        "BLE_BOND_ADMINISTRATION_CAPABILITY",
        "class BleBondInfo",
        "class BleBondList",
        "class BleBondRemoveResult",
        "validate_ble_bond_list",
        "validate_ble_bond_remove_result",
        "build_ble_bond_remove_frame",
    ):
        assert required in host_protocol
    assert "def ble_bond_list(" in host_client
    assert "def ble_bond_remove(" in host_client
    assert '"ble-bond-list"' in host_cli
    assert '"ble-bond-remove"' in host_cli

    print("PASS: BLE bond administration exact-deletion/static privacy contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
