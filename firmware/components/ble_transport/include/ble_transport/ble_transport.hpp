#pragma once

#include <atomic>
#include <cstdint>

#include "ble_security/ble_security.hpp"
#include "ble_transport/lifecycle_watchdog.hpp"
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
    bool security_teardown_already_disconnected(
        std::int32_t disconnect_result) const override;
    std::int32_t arm_ble_route_release_grace(
        hid_control_executor::BleRouteReleaseIdentity identity) override;
    void cancel_ble_route_release_grace(
        hid_control_executor::BleRouteReleaseIdentity identity) override;
    std::int32_t terminate_orphan_connection(
        std::uint16_t connection_handle) override;
    std::int32_t configure_connection(
        std::uint16_t connection_handle) override;
    std::int32_t initiate_security(
        std::uint16_t connection_handle) override;
    std::int32_t inject_passkey(std::uint16_t connection_handle,
                                std::uint32_t passkey) override;
    std::uint64_t monotonic_time_us() const override;
    void arm_pairing_timeout(ble_lifecycle::Generation generation,
                             std::uint16_t connection_handle,
                             std::uint32_t pairing_id) override;
    void cancel_pairing_timeout() override;
    void begin_security(ble_lifecycle::Generation generation,
                        std::uint16_t connection_handle) override;
    void refresh_security(std::uint16_t connection_handle,
                          bool identity_resolved_event = false) override;
    void retire_security(ble_lifecycle::Generation generation,
                         std::uint16_t connection_handle) override;
    void mark_security_unhealthy(
        ble_lifecycle::Generation generation) override;
    void apply_store_failure(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle,
        ble_security::StoreFailureKind kind, std::int32_t status) override;
    void apply_persistent_store_failure(
        ble_security::StoreFailureKind kind, std::int32_t status) override;
    bool persistent_store_failure_observed() const override;
    void record_heap_checkpoint(HeapCheckpoint checkpoint) override;

    ble_security::Snapshot security_snapshot() const override;
    bool security_ready_for_hid(ble_lifecycle::Generation generation,
                                std::uint16_t connection_handle) const override;
    hid_control_executor::GattSchemaStoreResult gatt_schema_status(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) override;
    hid_control_executor::GattSchemaStoreResult persist_gatt_schema_current(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) override;
    bool gatt_schema_current_for_hid(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) const override;
    std::uint16_t service_changed_value_handle() const override;
    std::int32_t request_gatt_cache_refresh(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle, std::uint16_t start_handle,
        std::uint16_t end_handle) override;
    hid_control_executor::BleBondListResult list_bonds() override;
    hid_control_executor::BleBondRemoveResult remove_bond(
        const hid_control_executor::BondId &bond_id) override;

    static void on_sync();
    static void on_reset(int reason);
    static int on_gap_event(struct ble_gap_event *event, void *context);

  private:
    using LifecycleTimeoutPurpose = detail::LifecycleWatchdogPurpose;

    bool signal(hid_control_executor::BleEventKind kind,
                std::uint16_t connection_handle, std::int32_t status);
    static void host_task(void *context);
    static void timeout_callback(void *context);
    static void pairing_timeout_callback(void *context);
    static void route_release_grace_callback(void *context);
    std::int32_t arm_timeout(std::uint64_t microseconds,
                             LifecycleTimeoutPurpose purpose);
    void cancel_timeout(LifecycleTimeoutPurpose purpose);
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
    std::atomic_bool gatt_schema_current_{false};
    std::atomic<std::uint16_t> service_changed_value_handle_{0};
    ble_security::ReadinessInhibit security_inhibit_{};
    ble_security::State security_{};
    ble_store_read_fn *original_store_read_ = nullptr;
    ble_store_write_fn *original_store_write_ = nullptr;
    ble_store_delete_fn *original_store_delete_ = nullptr;
    std::uint8_t own_address_type_ = 0;
    bool initialized_ = false;
    esp_timer_handle_t timeout_timer_ = nullptr;
    detail::LifecycleWatchdogOwnership timeout_ownership_{};
    esp_timer_handle_t pairing_timer_ = nullptr;
    std::atomic<ble_lifecycle::Generation> pairing_timer_generation_{0};
    std::atomic<std::uint16_t> pairing_timer_connection_{
        ble_lifecycle::kNoConnection};
    std::atomic<std::uint32_t> pairing_timer_id_{0};
    esp_timer_handle_t route_release_timer_ = nullptr;
    std::atomic<hid_runtime::AuthorityEpoch> route_release_authority_epoch_{0};
    std::atomic<hid_runtime::RouteGeneration> route_release_route_generation_{0};
    std::atomic<ble_lifecycle::Generation> route_release_ble_generation_{0};
    std::atomic<std::uint16_t> route_release_connection_{
        ble_lifecycle::kNoConnection};
    std::atomic<std::uint16_t> route_release_keyboard_handle_{0};
    std::atomic<std::uint16_t> route_release_mouse_handle_{0};
    std::atomic<std::uint32_t> route_release_epoch_{0};
    std::atomic_bool route_release_timer_active_{false};
};

}  // namespace ble_transport
