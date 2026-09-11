#include "uart_control_transport/uart_control_transport.hpp"
#include "uart_control_transport/deferred_hid_failure.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <unistd.h>

#include "control_framing/control_framing.hpp"
#include "bootloader_random.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "secure_memory/secure_memory.hpp"
#include "sensitive_request/sensitive_request.hpp"
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
std::atomic_bool s_lifecycle_invalidation_pending{false};
portMUX_TYPE s_local_owner_mux = portMUX_INITIALIZER_UNLOCKED;
control_session::LocalOwnerId s_published_local_owner = 0;
uart_control_transport::DeferredHidFailure s_pending_hid_failure;
bool s_started = false;

void fill_random(void *, std::uint8_t *output, std::size_t length) {
    // ESP-IDF v5.5.4 esp_fill_random() has no error return. The values are
    // protocol epoch markers, not authentication secrets.
    esp_fill_random(output, length);
}

bool fill_secure_random(void *, std::uint8_t *output, std::size_t length) {
    // The UART RX task does not exist yet and BLE/RF is still lazy-uninitialized.
    // ESP-IDF permits this bounded entropy-source window when RF and ADC users
    // are absent.
    bootloader_random_enable();
    esp_fill_random(output, length);
    bootloader_random_disable();
    return true;
}

bool hmac_sha256(void *, const std::uint8_t *key, std::size_t key_length,
                 const std::uint8_t *input, std::size_t input_length,
                 std::uint8_t output[sensitive_request::kDigestBytes]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr &&
           mbedtls_md_hmac(info, key, key_length, input, input_length, output) == 0;
}

bool write_protocol_frame(void *, const std::uint8_t *data, std::size_t length) {
    return uart_control_transport::write_machine(data, length);
}

std::uint64_t monotonic_now(void *) {
    return static_cast<std::uint64_t>(esp_timer_get_time());
}

void publish_current_local_owner() {
    const control_session::LocalOwnerId owner = s_protocol.local_owner_id();
    portENTER_CRITICAL(&s_local_owner_mux);
    s_published_local_owner = owner;
    portEXIT_CRITICAL(&s_local_owner_mux);
}

void consume_framing_event(void *, const control_framing::Event &event) {
    s_protocol.handle_framing_event(event);
    publish_current_local_owner();
}

void service_pending_notifications() {
    if (s_lifecycle_invalidation_pending.exchange(false, std::memory_order_acq_rel)) {
        s_protocol.on_hid_lifecycle_invalidation();
        publish_current_local_owner();
        // Discard only a failure captured for an owner that the lifecycle
        // pass has retired. A concurrent failure for the still-current owner
        // remains pending for the next serialized pass.
        portENTER_CRITICAL(&s_local_owner_mux);
        s_pending_hid_failure.discard_if_not_current(s_published_local_owner);
        portEXIT_CRITICAL(&s_local_owner_mux);
        return;
    }
    control_session::LocalOwnerId failed_owner = 0;
    portENTER_CRITICAL(&s_local_owner_mux);
    const bool failure_pending = s_pending_hid_failure.take(&failed_owner);
    portEXIT_CRITICAL(&s_local_owner_mux);
    if (failure_pending) {
        s_protocol.on_hid_safety_failure(failed_owner);
        publish_current_local_owner();
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
        service_pending_notifications();
        s_protocol.service();
        publish_current_local_owner();
        if (bytes_read > 0) {
            s_transport.consume(buffer.data(),
                                static_cast<std::size_t>(bytes_read),
                                consume_framing_event,
                                nullptr);
            secure_memory::zero(buffer.data(), static_cast<std::size_t>(bytes_read));
            // A callback can publish an authority epoch between two frames in
            // this same RX batch. Each request has its own epoch barrier;
            // this second pass makes cache/session cleanup prompt as well.
            service_pending_notifications();
        }
    }
}

}  // namespace

namespace uart_control_transport {

bool write_machine(const std::uint8_t *data, std::size_t length) {
    if (data == nullptr || length == 0 || length > kMaxLogicalMachineFrameBytes) {
        return false;
    }

    ::flockfile(stdout);
    const bool flushed = std::fflush(stdout) == 0;
    const int stdout_fd = ::fileno(stdout);
    const ssize_t written = flushed && stdout_fd >= 0
                                ? ::write(stdout_fd, data, length)
                                : -1;
    ::funlockfile(stdout);
    return written == static_cast<ssize_t>(length);
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
    configured_protocol.now = monotonic_now;
    configured_protocol.now_context = nullptr;
    if (!s_protocol.initialize(configured_protocol, fill_random, nullptr,
                               fill_secure_random, nullptr, hmac_sha256,
                               nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    publish_current_local_owner();

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

void on_hid_lifecycle_invalidation() {
    if (s_started) {
        // This callback must remain non-blocking and does not emit a machine
        // frame. The runtime epoch already blocks old authority immediately;
        // the RX task performs serialized cache/session maintenance.
        s_lifecycle_invalidation_pending.store(true, std::memory_order_release);
    }
}

void on_hid_safety_failure(
    control_session::LocalOwnerId originating_local_owner_id) {
    if (s_started) {
        // The exact report token already supplied its immutable source owner.
        // Keep the 64-bit deferred event under one bounded critical section.
        // A genuine failure for the current owner supersedes a stale-owner or
        // internal pending event; a late stale event cannot displace current
        // owner maintenance.
        portENTER_CRITICAL(&s_local_owner_mux);
        s_pending_hid_failure.publish(originating_local_owner_id,
                                      s_published_local_owner);
        portEXIT_CRITICAL(&s_local_owner_mux);
    }
}

}  // namespace uart_control_transport
