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
// It owns byte transport and the sole machine-response writer; the U2 protocol
// core handles bounded JSON/session commands and has no HID command dispatch.
esp_err_t start(const control_protocol::Config *protocol_config);

// Requests revocation of the active control session and retry caches. The USB
// lifecycle owner calls this only for native USB HID detach; it performs no
// HID action or blocking work in the callback.
void on_usb_unmount();

// Writes a bounded machine-readable frame without routing it through ESP_LOG
// or printf. Future protocol responses and events must use this sole path.
bool write_machine(const std::uint8_t *data, std::size_t length);

}  // namespace uart_control_transport
