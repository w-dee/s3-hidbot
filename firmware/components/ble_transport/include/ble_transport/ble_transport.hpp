#pragma once

#include <atomic>
#include <cstdint>

#include "hid_control_executor/hid_control_executor.hpp"
#include "esp_timer.h"

struct ble_gap_event;

namespace ble_transport {

// Construction is inert. NVS, controller, NimBLE host, and radio startup are
// deferred until the shared control task executes an accepted first enable.
class Backend final : public hid_control_executor::BleBackend {
  public:
    std::int32_t initialize(hid_control_executor::BleEventSink *sink,
                            hid_control_executor::BleDatabase *database,
                            ble_lifecycle::Generation generation) override;
    void set_generation(ble_lifecycle::Generation generation) override;
    std::int32_t start_advertising() override;
    std::int32_t stop_advertising() override;
    std::int32_t disconnect(std::uint16_t connection_handle) override;

    static void on_sync();
    static void on_reset(int reason);
    static int on_gap_event(struct ble_gap_event *event, void *context);

  private:
    bool signal(hid_control_executor::BleEventKind kind,
                std::uint16_t connection_handle, std::int32_t status);
    static void host_task(void *context);
    static void timeout_callback(void *context);
    void arm_timeout(std::uint64_t microseconds);
    void cancel_timeout();

    static Backend *instance_;
    hid_control_executor::BleEventSink *sink_ = nullptr;
    std::atomic<ble_lifecycle::Generation> generation_{0};
    std::uint8_t own_address_type_ = 0;
    bool initialized_ = false;
    esp_timer_handle_t timeout_timer_ = nullptr;
};

}  // namespace ble_transport
