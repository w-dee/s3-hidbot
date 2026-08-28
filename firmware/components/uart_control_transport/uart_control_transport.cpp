#include "uart_control_transport/uart_control_transport.hpp"

#include <array>
#include <cstdio>

#include "control_framing/control_framing.hpp"
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
bool s_started = false;

void consume_framing_event(void *, const control_framing::Event &) {
    // U1 establishes byte transport only. A later protocol layer will consume
    // complete-frame and overlong-frame events without changing sync semantics.
}

void control_rx_task(void *) {
    std::array<std::uint8_t, kRxReadChunkBytes> buffer{};
    while (true) {
        const int bytes_read = uart_read_bytes(kConsoleUart,
                                               buffer.data(),
                                               buffer.size(),
                                               kRxReadWaitTicks);
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

esp_err_t start() {
    if (s_started) {
        return ESP_OK;
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

}  // namespace uart_control_transport
