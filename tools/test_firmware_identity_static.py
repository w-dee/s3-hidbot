#!/usr/bin/env python3
"""Static guards for the U6.2C2 ESP-IDF identity producer boundary."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "firmware/main/firmware_identity_adapter.cpp"
ADAPTER_HEADER = ROOT / "firmware/main/firmware_identity_adapter.hpp"
MAIN = ROOT / "firmware/main/main.cpp"
MAIN_CMAKE = ROOT / "firmware/main/CMakeLists.txt"
PROTOCOL = ROOT / "firmware/components/control_protocol/control_protocol.cpp"
PROTOCOL_HEADER = ROOT / "firmware/components/control_protocol/include/control_protocol/control_protocol.hpp"
PROTOCOL_CMAKE = ROOT / "firmware/components/control_protocol/CMakeLists.txt"


def main() -> int:
    adapter = ADAPTER.read_text(encoding="utf-8")
    adapter_header = ADAPTER_HEADER.read_text(encoding="utf-8")
    main_source = MAIN.read_text(encoding="utf-8")
    main_cmake = MAIN_CMAKE.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    protocol_header = PROTOCOL_HEADER.read_text(encoding="utf-8")
    protocol_cmake = PROTOCOL_CMAKE.read_text(encoding="utf-8")

    assert "esp_app_get_description()" in adapter
    assert "description == nullptr" in adapter
    assert "ESP_APP_DESC_MAGIC_WORD" in adapter
    assert "bounded_descriptor_string" in adapter
    assert "const volatile std::uint8_t *source" in adapter
    assert "raw_digest[index] = source[index]" in adapter
    assert "configured_source_revision()" in adapter
    assert "build_identity" in adapter
    assert "kBuildProfile" in adapter
    assert "esp_app_get_elf_sha256(" not in adapter
    assert "esp_app_get_elf_sha256_str" not in adapter
    assert "identity_build_config" not in adapter

    assert "build_runtime_identity" in adapter_header
    assert "firmware_identity_adapter::build_runtime_identity" in main_source
    assert "std::abort()" in main_source
    assert "firmware_identity::Identity s_firmware_identity" in main_source
    assert ".firmware_identity = &s_firmware_identity" in main_source
    app_main_position = main_source.index('extern "C" void app_main()')
    app_main = main_source[app_main_position:]
    identity_position = app_main.index("build_runtime_identity")
    lifecycle_position = app_main.index("s_usb_exposure.initialize")
    transport_position = app_main.index("uart_control_transport::start")
    assert identity_position < lifecycle_position < transport_position
    # U7.1B keeps native USB absent at boot: only the dedicated lifecycle
    # backend may contain the public install call, after UART startup setup.
    assert "tinyusb_driver_install" not in app_main
    assert "hid_control_executor" in main_cmake
    assert '"firmware_identity_adapter.cpp"' in main_cmake
    assert "esp_app_format" in main_cmake

    assert "firmware_identity::Identity" in protocol_header
    assert "firmware_identity" in protocol_cmake
    assert "kIdentityCapabilityJson" in protocol
    assert "firmware.identity-v1" in protocol
    assert "kMaximumInfoResponseBytes" in protocol
    assert "is_valid_identity" in protocol
    assert r'\"source_revision\":null' in protocol
    assert "app_elf_sha256" in protocol

    print("PASS: firmware identity producer static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
