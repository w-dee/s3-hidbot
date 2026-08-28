#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace uart_control_transport {

inline constexpr std::size_t kMaxMachineFrameBytes = 1024;

// Starts the sole UART RX consumer for the configured ESP-IDF console UART.
// U1 consumes only transport/framing bytes; it intentionally has no JSON or
// HID command dispatcher.
esp_err_t start();

// Writes a bounded machine-readable frame without routing it through ESP_LOG
// or printf. Future protocol responses and events must use this sole path.
bool write_machine(const std::uint8_t *data, std::size_t length);

}  // namespace uart_control_transport
