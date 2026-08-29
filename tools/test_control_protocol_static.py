#!/usr/bin/env python3
"""Static U2 boundaries: protocol JSON must not directly control HID or UART."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "firmware/components/control_protocol/control_protocol.cpp"
PROTOCOL_HEADER = ROOT / "firmware/components/control_protocol/include/control_protocol/control_protocol.hpp"
SESSION = ROOT / "firmware/components/control_session/control_session.cpp"
TRANSPORT = ROOT / "firmware/components/uart_control_transport/uart_control_transport.cpp"
MAIN = ROOT / "firmware/main/blink.cpp"


def main() -> int:
    protocol = PROTOCOL.read_text(encoding="utf-8")
    protocol_header = PROTOCOL_HEADER.read_text(encoding="utf-8")
    session = SESSION.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    main_source = MAIN.read_text(encoding="utf-8")

    assert "config_.output(config_.output_context" in protocol
    assert "request_json_scratch_" in protocol_header
    assert "response_scratch_" in protocol_header
    assert "prepare_response_scratch()" in protocol
    assert "control_session::ResponseFrame response{}" not in protocol
    assert "char json[control_session::kMaxRequestBytes + 1]" not in protocol
    assert "struct ResponseSession" in protocol
    assert "kUncorrelatableSession" in protocol
    assert "\\\"session\\\":%s" in protocol
    assert "\\\"client_nonce\\\":\\\"%s\\\"" in protocol
    for source in (protocol, session):
        assert "uart_write_bytes" not in source
        assert "ESP_LOG" not in source
        assert "std::printf(" not in source
        assert "::printf(" not in source
        assert "tud_hid_n_mouse_report" not in source
        assert "tud_hid_n_keyboard_report" not in source
        assert "gpio_config" not in source
        assert "GPIO_NUM_19" not in source
        assert "GPIO_NUM_20" not in source

    assert "write_protocol_frame" in transport
    assert "return uart_control_transport::write_machine(data, length);" in transport
    assert "s_protocol.handle_framing_event(event);" in transport
    assert "s_lifecycle_invalidation_pending.store(true, std::memory_order_release);" in transport
    assert "s_protocol.on_hid_lifecycle_invalidation();" in transport
    assert "authority_epoch_provider" in protocol_header
    assert "AuthorityEpochProvider" in protocol_header
    assert "session_.inspect_request(session, id, payload, authority_epoch" in protocol
    assert "session_.inspect_hello(client_nonce, payload, authority_epoch" in protocol
    assert "session_authority_epoch_" in session
    assert "hello_cache_.authority_epoch != current_epoch" in session
    assert "request_cache_.authority_epoch != current_epoch" in session
    assert "session_authority_epoch_ != current_epoch" in session
    assert "status_snapshot()" in main_source
    assert "service_sof()" in main_source
    assert "on_hid_safety_failure" in protocol
    assert "hid_safety_failure" in protocol_header
    assert 'command == "hid.release_all"' in protocol
    assert "HID_SAFETY_PENDING" in protocol
    assert "hid.release-all-v1" in protocol
    assert "make_release_all" in protocol
    assert "release_all_provider" in protocol_header
    assert 'command == "hid.keyboard.report"' in protocol
    assert "hid.keyboard-report-v1" in protocol
    assert 'command == "hid.mouse.report"' in protocol
    assert "hid.mouse-report-v1" in protocol
    assert "keyboard_report_provider" in protocol_header
    assert "mouse_report_provider" in protocol_header
    assert "tud_hid_n_keyboard_report" not in main_source
    assert "tud_hid_n_report" not in main_source
    print("PASS: U2 protocol/HID/UART static boundaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
