#pragma once

#include <atomic>
#include <cstdint>

#include "ble_security/ble_security.hpp"
#include "hid_control_executor/hid_control_executor.hpp"
#include "esp_timer.h"
#include "host/ble_store.h"

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
    std::int32_t configure_connection(
        std::uint16_t connection_handle) override;
    void record_heap_checkpoint(HeapCheckpoint checkpoint) override;

    ble_security::Snapshot security_snapshot() const;
    bool security_ready_for_hid(ble_lifecycle::Generation generation,
                                std::uint16_t connection_handle) const;

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
    void refresh_security(std::uint16_t connection_handle,
                          bool identity_resolved_event = false);
    void observe_store_failure(ble_security::StoreFailureKind kind,
                               std::int32_t status,
                               bool persistent_store_unhealthy,
                               std::uint16_t connection_handle);
    static int store_read(int object_type, const union ble_store_key *key,
                          union ble_store_value *value);
    static int store_write(int object_type,
                           const union ble_store_value *value);
    static int store_delete(int object_type,
                            const union ble_store_key *key);
    static int store_status(struct ble_store_status_event *event, void *argument);

    static Backend *instance_;
    hid_control_executor::BleEventSink *sink_ = nullptr;
    hid_control_executor::BleDatabase *database_ = nullptr;
    std::atomic<ble_lifecycle::Generation> generation_{0};
    std::atomic<std::uint16_t> current_connection_{
        ble_lifecycle::kNoConnection};
    std::atomic_bool identity_resolved_{false};
    ble_security::State security_{};
    ble_store_read_fn *original_store_read_ = nullptr;
    ble_store_write_fn *original_store_write_ = nullptr;
    ble_store_delete_fn *original_store_delete_ = nullptr;
    std::uint8_t own_address_type_ = 0;
    bool initialized_ = false;
    esp_timer_handle_t timeout_timer_ = nullptr;
};

}  // namespace ble_transport
