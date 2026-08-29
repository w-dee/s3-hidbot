#pragma once

#include <cstddef>
#include <cstdint>

#include "control_protocol/control_protocol.hpp"
#include "esp_err.h"

namespace uart_control_transport {

// The formatter includes the prefix, JSON, and LF in this logical bound. The
// configured console VFS converts LF to CRLF on the UART wire, so the wire
// representation is one byte longer.
inline constexpr std::size_t kMaxLogicalMachineFrameBytes = 1023;
inline constexpr std::size_t kMaxWireMachineFrameBytes = 1024;

// Starts the sole UART RX consumer for the configured ESP-IDF console UART.
// It owns byte transport and the sole machine-response writer; the protocol
// core handles bounded JSON/session commands and the safety-only release_all
// dispatch. Unsafe HID report commands remain outside this transport.
esp_err_t start(const control_protocol::Config *protocol_config);

// Publishes eventual protocol/session cleanup after a native HID lifecycle
// authority boundary. The runtime's atomic authority epoch is the immediate
// correctness barrier; this callback stays non-blocking and performs no HID
// action. It is valid for suspend, resume, detach, and mount notifications.
void on_hid_lifecycle_invalidation();

// Requests protocol-task authority revocation after an input HID report
// failure. The TinyUSB callback only publishes an atomic notification; the
// RX task performs the session mutation and invokes the configured safety
// callback.
void on_hid_safety_failure();

// Writes a bounded machine-readable frame without routing it through ESP_LOG
// or printf. Future protocol responses and events must use this sole path.
bool write_machine(const std::uint8_t *data, std::size_t length);

}  // namespace uart_control_transport
