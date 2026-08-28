#include "uart_control_transport/uart_control_transport.hpp"

#include <array>
#include <atomic>
#include <cstdio>

#include "control_framing/control_framing.hpp"
#include "esp_random.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kLogTag[] = "uart_control";
constexpr uart_port_t kConsoleUart =
    static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr int kRxBufferBytes = 2048;
constexpr std::size_t kRxReadChunkBytes = 64;
constexpr std::uint32_t kRxTaskStackBytes = 4096;
constexpr UBaseType_t kRxTaskPriority = tskIDLE_PRIORITY + 2;
constexpr TickType_t kRxReadWaitTicks = pdMS_TO_TICKS(100);

control_framing::Transport s_transport;
control_protocol::Protocol s_protocol;
std::atomic_bool s_unmount_pending{false};
bool s_started = false;

void fill_random(void *, std::uint8_t *output, std::size_t length) {
    // ESP-IDF v5.5.4 esp_fill_random() has no error return. The values are
    // protocol epoch markers, not authentication secrets.
    esp_fill_random(output, length);
}

bool write_protocol_frame(void *, const std::uint8_t *data, std::size_t length) {
    return uart_control_transport::write_machine(data, length);
}

void consume_framing_event(void *, const control_framing::Event &event) {
    s_protocol.handle_framing_event(event);
}

void revoke_pending_session() {
    if (s_unmount_pending.exchange(false, std::memory_order_acq_rel)) {
        s_protocol.on_usb_unmount();
    }
}

void control_rx_task(void *) {
    std::array<std::uint8_t, kRxReadChunkBytes> buffer{};
    while (true) {
        const int bytes_read = uart_read_bytes(kConsoleUart,
                                               buffer.data(),
                                               buffer.size(),
                                               kRxReadWaitTicks);
        // The TinyUSB lifecycle callback only sets this atomic flag. Keeping
        // session mutation in this task avoids concurrent protocol-state access.
        revoke_pending_session();
        if (bytes_read > 0) {
            s_transport.consume(buffer.data(),
                                static_cast<std::size_t>(bytes_read),
                                consume_framing_event,
                                nullptr);
        }
    }
}

}  // namespace

namespace uart_control_transport {

bool write_machine(const std::uint8_t *data, std::size_t length) {
    if (data == nullptr || length == 0 || length > kMaxMachineFrameBytes) {
        return false;
    }

    ::flockfile(stdout);
    const bool flushed = std::fflush(stdout) == 0;
    const int written = flushed ? uart_write_bytes(kConsoleUart, data, length) : -1;
    ::funlockfile(stdout);
    return written == static_cast<int>(length);
}

esp_err_t start(const control_protocol::Config *protocol_config) {
    if (s_started) {
        return ESP_OK;
    }
    if (protocol_config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    control_protocol::Config configured_protocol = *protocol_config;
    configured_protocol.output = write_protocol_frame;
    configured_protocol.output_context = nullptr;
    if (!s_protocol.initialize(configured_protocol, fill_random, nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!uart_is_driver_installed(kConsoleUart)) {
        const esp_err_t install_result = uart_driver_install(kConsoleUart,
                                                              kRxBufferBytes,
                                                              0,
                                                              0,
                                                              nullptr,
                                                              0);
        if (install_result != ESP_OK) {
            return install_result;
        }
    }

    // Preserve the console-selected UART number, pins, and baud rate while
    // changing stdout/VFS to the already installed interrupt-driven driver.
    uart_vfs_dev_use_driver(kConsoleUart);

    const BaseType_t task_result = xTaskCreate(control_rx_task,
                                               "uart_control_rx",
                                               kRxTaskStackBytes,
                                               nullptr,
                                               kRxTaskPriority,
                                               nullptr);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(kLogTag, "S3-HIDBOT UART TRANSPORT READY");
    return ESP_OK;
}

void on_usb_unmount() {
    if (s_started) {
        // This callback must remain non-blocking and does not emit a machine
        // frame. The UART RX task applies the revoke before its next request.
        s_unmount_pending.store(true, std::memory_order_release);
    }
}

}  // namespace uart_control_transport
