#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ble_security/ble_security.hpp"
#include "ble_transport/lifecycle_watchdog.hpp"
#include "ble_transport/bond_association.hpp"
#include "ble_transport/route_release_grace_ownership.hpp"
#include "hid_control_executor/hid_control_executor.hpp"
#include "esp_timer.h"
#include "host/ble_store.h"
#include "nimble/nimble_npl.h"

struct ble_gap_event;

namespace ble_transport {

// Construction is inert. NVS, controller, NimBLE host, and radio startup are
// deferred until the shared control task executes an accepted first enable.
class Backend final : public hid_control_executor::BleBackend {
  public:
    std::int32_t initialize(hid_control_executor::BleEventSink *sink,
                            hid_control_executor::BleDatabase *database,
                            ble_lifecycle::Generation generation) override;
    bool configure_profile(ble_fixture_profile::ProfileId id,
                            std::uint32_t incarnation) override;
    std::uint64_t begin_stop() override;
    ble_lifecycle::StopStatus poll_stop(std::uint64_t id) const override;
    void expire_stop(std::uint64_t id) override;
    bool finish_stop(std::uint64_t id) override;
    void set_generation(ble_lifecycle::Generation generation) override;
    std::int32_t start_advertising() override;
    std::int32_t start_finite_advertising(
        std::uint16_t interval_units, std::uint32_t timeout_ms,
        std::uint64_t advertising_incarnation) override;
    std::int32_t stop_advertising() override;
    std::int32_t begin_hidden_exposure() override;
    bool physical_exposure_hidden() const override;
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
    std::int32_t set_connection_data_length(
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
    // Linker-wrapped pinned transport ingress. Successful peripheral
    // Connection Complete is owned before NimBLE host admission.
    static bool hci_callback_registration_allowed();
    static bool observe_hci_ingress(const std::uint8_t *event);
    static void queue_hci_establishment_resolution();

  private:
    using LifecycleTimeoutPurpose = detail::LifecycleWatchdogPurpose;

    std::int32_t start_advertising_internal(
        std::uint16_t interval_units, std::int32_t duration_ms,
        std::uint64_t advertising_incarnation);
    bool signal_event(hid_control_executor::BleEvent event);
    bool signal(hid_control_executor::BleEventKind kind,
                std::uint16_t connection_handle, std::int32_t status);
    static void host_task(void *context);
    static void stop_task(void *context);
    static void timer_barrier_callback(void *context);
    static void hidden_exposure_barrier_callback(struct ble_npl_event *event);
    static void hci_establishment_callback(struct ble_npl_event *event);
    void terminate_hidden_connection(std::uint16_t connection_handle);
    void resolve_hci_establishment(std::uint64_t establishment_id);
    void retire_hci_establishment();
    void process_hci_establishment();
    struct StopOperations;
    bool retire_timers_after_stop();
    ble_lifecycle::StopTransaction stop_transaction_{};
    std::uint64_t stop_worker_id_ = 0;
    std::atomic_bool host_exited_{false};
    std::atomic_bool timer_barrier_passed_{false};
    esp_timer_handle_t timer_barrier_ = nullptr;
    struct ble_npl_event hidden_exposure_barrier_{};
    bool hidden_exposure_barrier_initialized_ = false;
    struct ble_npl_event hci_establishment_event_{};
    bool hci_establishment_event_initialized_ = false;
    std::atomic<std::uint64_t> hci_establishment_id_{0};
    std::atomic<std::uint64_t> next_hci_establishment_id_{1};
    std::atomic<std::uint64_t> hci_establishment_disconnect_owner_{0};
    std::atomic<std::uint32_t> hci_establishment_stack_{0};
    std::atomic<ble_lifecycle::Generation> hci_establishment_generation_{0};
    std::atomic<std::uint16_t> hci_establishment_handle_{
        ble_lifecycle::kNoConnection};
    std::atomic_bool hci_ingress_enabled_{false};
    std::atomic<std::uint64_t> hci_ingress_epoch_{1};
    std::atomic_bool hidden_exposure_requested_{false};
    std::atomic_bool hidden_exposure_barrier_passed_{false};
    std::atomic_bool hidden_exposure_termination_claimed_{false};
    std::atomic<std::uint64_t> advertising_incarnation_{0};
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
    int restore_store_callbacks();
    int read_security_raw(bool our, const ble_store_key_sec &key, ble_store_value_sec &value) const;
    bool compatible_association(const ble_addr_t &identity) const;
    detail::AssociationCreation association_creation_{};
    std::uint64_t host_connection_incarnation_ = 0;
    std::uint64_t active_host_connection_ = 0;
    std::uint16_t host_connection_handle_ = ble_lifecycle::kNoConnection;
    ble_addr_t host_connection_identity_{};
    bool host_identity_valid_ = false;
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
    const ble_fixture_profile::ProfileDefinition *profile_ =
        &ble_fixture_profile::kStrictComposite;
    std::uint32_t stack_incarnation_ = 0;
    esp_timer_handle_t timeout_timer_ = nullptr;
    detail::LifecycleWatchdogOwnership timeout_ownership_{};
    esp_timer_handle_t pairing_timer_ = nullptr;
    std::atomic<ble_lifecycle::Generation> pairing_timer_generation_{0};
    std::atomic<std::uint16_t> pairing_timer_connection_{
        ble_lifecycle::kNoConnection};
    std::atomic<std::uint32_t> pairing_timer_id_{0};
    struct RouteReleaseTimerContext {
        Backend *backend = nullptr;
        std::size_t slot =
            detail::RouteReleaseGraceOwnership::kSlotCount;
    };
    std::array<esp_timer_handle_t,
               detail::RouteReleaseGraceOwnership::kSlotCount>
        route_release_timers_{};
    std::array<RouteReleaseTimerContext,
               detail::RouteReleaseGraceOwnership::kSlotCount>
        route_release_timer_contexts_{};
    detail::RouteReleaseGraceOwnership route_release_grace_ownership_{};
};

}  // namespace ble_transport
