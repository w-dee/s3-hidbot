#!/usr/bin/env python3
"""Static U2 boundaries: protocol JSON must not directly control HID or UART."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "firmware/components/control_protocol/control_protocol.cpp"
SESSION = ROOT / "firmware/components/control_session/control_session.cpp"
TRANSPORT = ROOT / "firmware/components/uart_control_transport/uart_control_transport.cpp"
MAIN = ROOT / "firmware/main/blink.cpp"


def main() -> int:
    protocol = PROTOCOL.read_text(encoding="utf-8")
    session = SESSION.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    main_source = MAIN.read_text(encoding="utf-8")

    assert "config_.output(config_.output_context" in protocol
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
    assert "s_unmount_pending.store(true, std::memory_order_release);" in transport
    assert "s_protocol.on_usb_unmount();" in transport
    assert "tud_hid_n_ready(kKeyboardInterface)" in main_source
    assert "tud_hid_n_ready(kMouseInterface)" in main_source
    assert "tud_hid_n_keyboard_report" not in main_source
    print("PASS: U2 protocol/HID/UART static boundaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
