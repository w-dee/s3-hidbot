#!/usr/bin/env python3
"""Static guards for the non-public U7.5A Slice B pairing controller."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    executor_header = (ROOT / "firmware/components/hid_control_executor/include/hid_control_executor/hid_control_executor.hpp").read_text()
    executor = (ROOT / "firmware/components/hid_control_executor/hid_control_executor.cpp").read_text()
    transport_header = (ROOT / "firmware/components/ble_transport/include/ble_transport/ble_transport.hpp").read_text()
    transport = (ROOT / "firmware/components/ble_transport/ble_transport.cpp").read_text()
    pairing_header = (ROOT / "firmware/components/ble_pairing/include/ble_pairing/ble_pairing.hpp").read_text()
    protocol = (ROOT / "firmware/components/control_protocol/control_protocol.cpp").read_text()

    assert "constexpr std::uint32_t kInputTimeoutMs = 25000;" in pairing_header
    assert "enum class LiveState" in pairing_header
    for state in ("kIdle", "kSecuring", "kWaitingInput"):
        assert state in pairing_header
    for result in ("kSucceeded", "kSmpFailed", "kTimeout",
                   "kPeerDisconnected", "kStoreFull", "kStorage",
                   "kQueueOverflow", "kRepeatPairing", "kSecurityPolicy"):
        assert result in pairing_header

    event = re.search(r"struct BleEvent \{(.*?)\n\};", executor_header, re.S)
    assert event
    assert set(re.findall(r"\b(kind|generation|connection_handle|status|pairing_id)\b",
                          event.group(1))) == {
        "kind", "generation", "connection_handle", "status", "pairing_id"
    }
    assert not re.search(r"\b(passkey|secret|ltk|irk|address)\b", event.group(1), re.I)
    assert "static constexpr std::size_t kActionQueueDepth = 12;" in executor_header
    assert "native_queue_[kActionQueueDepth]" in executor_header
    assert "xQueueSend(s_action_queue, &item, 0)" in executor
    assert "mark_ble_event_overflow(event);" in executor
    assert "overflow_authority_.compare_exchange_weak(" in executor
    assert "ble_event_overflow_pending(lifecycle.generation)" in executor
    assert "ble_lifecycle_handoff_failure_.store(true" in executor
    assert "reconcile_ble_lifecycle_handoff_failure()" in executor
    for obsolete in ("std::atomic_bool ble_event_overflow_{",
                     "overflow_generation_", "overflow_connection_"):
        assert obsolete not in executor_header
        assert obsolete not in executor

    consume = re.search(
        r"bool Controller::consume_ble_overflow\(\) \{(.*?)"
        r"\n\}\n\nvoid Controller::terminate_security_connection",
        executor, re.S)
    assert consume
    assert "ble_event_overflow_pending(authority)" in consume.group(1)
    assert "fail_ble(" in consume.group(1)
    assert consume.group(1).index("ble_event_overflow_pending(authority)") < \
        consume.group(1).index("fail_ble(")
    assert "compare_exchange_strong(" in consume.group(1)

    process = re.search(
        r"void Controller::process\(Action action\) \{(.*?)"
        r"\n\}\n\nvoid Controller::fail_ble",
        executor, re.S)
    assert process
    assert "persistent_store_failure_observed()" in process.group(1)
    assert "commit_persistent_store_failure(" in process.group(1)
    assert process.group(1).index("persistent_store_failure_observed()") < \
        process.group(1).index("reconcile_ble_lifecycle_handoff_failure()")
    assert process.group(1).index("reconcile_ble_lifecycle_handoff_failure()") < \
        process.group(1).index("consume_ble_overflow()")
    fatal_commit = re.search(
        r"void Controller::commit_persistent_store_failure\((.*?)"
        r"\n\}\n\nbool Controller::consume_ble_overflow",
        executor, re.S)
    assert fatal_commit
    assert "apply_persistent_store_failure(" in fatal_commit.group(1)
    assert "fail_ble(" in fatal_commit.group(1)
    assert "persistent_store_failure_committed_ = true" in fatal_commit.group(1)

    sync = re.search(
        r"void Backend::on_sync\(\) \{(.*?)\n\}", transport, re.S)
    assert sync
    assert sync.group(1).index("instance_->signal(") < \
        sync.group(1).index("signal_ble_lifecycle_handoff_failure()") < \
        sync.group(1).index("instance_->cancel_timeout(")
    assert "LifecycleTimeoutPurpose::kSync" in sync.group(1)
    reset = re.search(
        r"void Backend::on_reset\(int reason\) \{(.*?)\n\}", transport, re.S)
    assert reset
    assert "!published || timeout_result != ESP_OK" in reset.group(1)
    assert "signal_ble_lifecycle_handoff_failure()" in reset.group(1)
    timeout = re.search(
        r"void Backend::timeout_callback\(void \*context\) \{(.*?)\n\}",
        transport, re.S)
    assert timeout
    assert "timeout_purpose_.exchange(" in timeout.group(1)
    assert timeout.group(1).index("backend->signal(") < \
        timeout.group(1).index("signal_ble_lifecycle_handoff_failure()")
    disconnect = re.search(
        r"case BLE_GAP_EVENT_DISCONNECT:(.*?)break;", transport, re.S)
    assert disconnect
    assert disconnect.group(1).index("backend->signal(") < \
        disconnect.group(1).index("backend->cancel_timeout(")
    assert "LifecycleTimeoutPurpose::kDisconnect" in disconnect.group(1)
    assert "std::atomic<LifecycleTimeoutPurpose>::is_always_lock_free" in \
        transport_header

    disable_request = re.search(
        r"BleCommandOutcome Controller::request_ble_disable\(\) \{(.*?)"
        r"\n\}\n\nble_lifecycle::Snapshot Controller::ble_snapshot",
        executor, re.S)
    assert disable_request
    for forbidden in ("retire_security", "begin_security", "refresh_security",
                      "mark_security_unhealthy", "apply_store_failure",
                      "apply_persistent_store_failure", "security_snapshot"):
        assert forbidden not in disable_request.group(1)

    compound_mutations = (
        "begin_connection", "retire_connection", "apply_store_failure",
        "apply_persistent_store_failure", "apply_verification",
        "mark_lifecycle_unhealthy",
    )
    production_sources = list((ROOT / "firmware/components").rglob("*.cpp"))
    for mutation in compound_mutations:
        direct_owners = {
            path.relative_to(ROOT).as_posix()
            for path in production_sources
            if re.search(rf"\bsecurity_\s*\.\s*{mutation}\s*\(",
                         path.read_text())
        }
        assert direct_owners <= {
            "firmware/components/ble_transport/ble_transport.cpp"
        }

    assert "BLE_GAP_EVENT_PASSKEY_ACTION" in transport
    assert "event->passkey.params.action == BLE_SM_IOACT_INPUT" in transport
    assert "BLE_GAP_EVENT_REPEAT_PAIRING" in transport
    assert "return BLE_GAP_REPEAT_PAIRING_IGNORE;" in transport
    assert "ble_store_util_delete_peer" not in transport
    assert transport.count("ble_gap_security_initiate(connection_handle)") == 1
    assert transport.count("ble_sm_inject_io(connection_handle, &input)") == 1
    assert "input.action = BLE_SM_IOACT_INPUT;" in transport
    assert "ble_pairing::kInputTimeoutMs" in transport
    assert "cJSON" not in transport

    for exposed in ("ble.pairing.status", "ble.pairing.respond",
                    "ble.pairing-transaction-v1"):
        assert exposed in protocol
    assert protocol.count("ble.pairing-transaction-v1") == 2
    assert "ble.pairing-control-v1" not in protocol
    assert "ble.bond-store-v1" not in protocol

    action = re.search(r"struct Action \{(.*?)\n    \};", executor_header, re.S)
    assert action
    assert "mailbox_token" in action.group(1)
    assert not re.search(r"\b(passkey|secret)\b", action.group(1), re.I)
    assert "secure_memory::zero(&pairing_mailbox_" in executor
    assert "reconcile_pairing_deadline();" in executor

    project_ble = "\n".join(path.read_text() for path in
        (ROOT / "firmware/components").glob("ble_*/*.cpp"))
    for call in re.findall(r"ESP_LOG[A-Z]+\([^;]+;", project_ble):
        assert not re.search(r"(passkey|secret|ltk|irk|peer.*addr)", call, re.I)

    print("PASS: BLE pairing transaction static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
