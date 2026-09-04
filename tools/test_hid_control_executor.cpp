#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "ble_hid_service/ble_hid_service.hpp"
#include "hid_control_executor/hid_control_executor.hpp"

namespace {

using hid_control_executor::ControlOperation;

struct FakeBackend final : hid_control_executor::Backend {
    hid_control_executor::BackendResult install() override {
        ++install_calls;
        return install_result;
    }

    hid_control_executor::BackendResult uninstall() override {
        ++uninstall_calls;
        return uninstall_result;
    }

    int install_calls = 0;
    int uninstall_calls = 0;
    hid_control_executor::BackendResult install_result{
        .kind = hid_control_executor::BackendResultKind::kSuccess,
        .error_code = 0,
    };
    hid_control_executor::BackendResult uninstall_result{
        .kind = hid_control_executor::BackendResultKind::kSuccess,
        .error_code = 0,
    };
};

struct FakeBleBackend final : hid_control_executor::BleBackend {
    std::int32_t initialize(hid_control_executor::BleEventSink *event_sink,
                            hid_control_executor::BleDatabase *database,
                            ble_lifecycle::Generation generation) override {
        ++initialize_calls;
        sink = event_sink;
        active_database = database;
        active_generation = generation;
        if (initialize_persistent_failure) {
            (void)security_inhibit.inhibit(
                generation, ble_lifecycle::kNoConnection, true);
            (void)event_sink->signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kStorageFailure,
                .generation = generation,
                .connection_handle = ble_lifecycle::kNoConnection,
                .status = initialize_result,
                .store_failure_kind =
                    ble_security::StoreFailureKind::kRead,
            });
        }
        if (initialize_result != 0) {
            return initialize_result;
        }
        if (active_database == nullptr) return -1;
        active_database->bind_event_sink(event_sink);
        active_database->set_generation(generation);
        return active_database->register_database();
    }
    void set_generation(ble_lifecycle::Generation generation) override {
        active_generation = generation;
        if (active_database != nullptr) active_database->set_generation(generation);
    }
    std::int32_t start_advertising() override {
        ++advertising_attempts;
        const int validation_result = active_database == nullptr
                                          ? -1
                                          : active_database->validate_registered_database();
        if (validation_result != 0) {
            return validation_result;
        }
        if (advertising_result == 0) {
            ++advertising_calls;
        }
        return advertising_result;
    }
    std::int32_t stop_advertising() override {
        ++stop_calls;
        return stop_result;
    }
    std::int32_t disconnect(std::uint16_t connection_handle) override {
        ++disconnect_calls;
        last_connection = connection_handle;
        return disconnect_result;
    }
    bool security_teardown_already_disconnected(
        std::int32_t result) const override {
        return result == already_disconnected_result;
    }
    std::int32_t arm_ble_route_release_grace(
        hid_control_executor::BleRouteReleaseIdentity identity) override {
        ++arm_route_release_grace_calls;
        route_release_identity = identity;
        route_release_grace_armed = route_release_grace_result == 0;
        return route_release_grace_result;
    }
    void cancel_ble_route_release_grace(
        hid_control_executor::BleRouteReleaseIdentity identity) override {
        ++cancel_route_release_grace_calls;
        if (identity.authority_epoch ==
                route_release_identity.authority_epoch &&
            identity.route_generation ==
                route_release_identity.route_generation &&
            identity.ble_generation == route_release_identity.ble_generation &&
            identity.connection_handle ==
                route_release_identity.connection_handle &&
            identity.release_epoch == route_release_identity.release_epoch) {
            route_release_grace_armed = false;
        }
    }
    std::int32_t terminate_orphan_connection(
        std::uint16_t connection_handle) override {
        ++orphan_terminate_calls;
        last_orphan_connection = connection_handle;
        return orphan_terminate_result;
    }
    std::int32_t configure_connection(std::uint16_t connection_handle) override {
        ++configure_connection_calls;
        last_configured_connection = connection_handle;
        return configure_connection_result;
    }
    std::int32_t initiate_security(std::uint16_t connection_handle) override {
        ++initiate_security_calls;
        last_connection = connection_handle;
        return initiate_security_result;
    }
    std::int32_t inject_passkey(std::uint16_t connection_handle,
                                std::uint32_t passkey) override {
        ++inject_calls;
        last_connection = connection_handle;
        last_injected_value = passkey;
        return inject_result;
    }
    std::uint64_t monotonic_time_us() const override { return now_us; }
    void arm_pairing_timeout(ble_lifecycle::Generation generation,
                             std::uint16_t connection_handle,
                             std::uint32_t pairing_id) override {
        ++arm_pairing_timeout_calls;
        timer_generation = generation;
        timer_connection = connection_handle;
        timer_pairing_id = pairing_id;
    }
    void cancel_pairing_timeout() override {
        ++cancel_pairing_timeout_calls;
        timer_pairing_id = 0;
    }
    void begin_security(ble_lifecycle::Generation generation,
                        std::uint16_t connection_handle) override {
        gatt_schema_current = false;
        security_inhibit.begin_connection(generation, connection_handle);
        security.begin_connection(generation, connection_handle);
    }
    void refresh_security(std::uint16_t connection_handle,
                          bool identity_resolved_event = false) override {
        ++refresh_security_calls;
        if (identity_resolved_event) security_link.identity_resolved = true;
        security.apply_verification(active_generation, connection_handle,
                                    security_link, security_persisted);
        if (adopt_stored_schema_during_security_refresh &&
            security.security_ready_for_hid(active_generation,
                                            connection_handle)) {
            gatt_schema_current = stored_gatt_schema_current;
        }
    }
    void retire_security(ble_lifecycle::Generation generation,
                         std::uint16_t connection_handle) override {
        ++retire_security_calls;
        security_inhibit.retire_connection(generation, connection_handle);
        security.retire_connection(generation, connection_handle);
        gatt_schema_current = false;
    }
    void mark_security_unhealthy(
        ble_lifecycle::Generation generation) override {
        security.mark_lifecycle_unhealthy(generation);
    }
    void apply_store_failure(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle,
        ble_security::StoreFailureKind kind, std::int32_t status) override {
        ++apply_store_failure_calls;
        security.apply_store_failure(generation, connection_handle, kind, status);
    }
    void apply_persistent_store_failure(
        ble_security::StoreFailureKind kind, std::int32_t status) override {
        ++apply_persistent_store_failure_calls;
        security.apply_persistent_store_failure(kind, status);
    }
    bool persistent_store_failure_observed() const override {
        return security_inhibit.persistent_failure_observed();
    }
    ble_security::Snapshot security_snapshot() const override {
        auto snapshot = security.snapshot();
        if (snapshot.coherent && security_inhibit.inhibits(
                                     snapshot.generation,
                                     snapshot.connection_handle)) {
            snapshot.project_verified_bond_persisted = false;
            snapshot.store_healthy = false;
        }
        return snapshot;
    }
    bool security_ready_for_hid(ble_lifecycle::Generation generation,
                                std::uint16_t connection_handle) const override {
        return !security_inhibit.inhibits(generation, connection_handle) &&
               security.security_ready_for_hid(generation, connection_handle);
    }
    hid_control_executor::GattSchemaStoreResult gatt_schema_status(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) override {
        ++gatt_schema_status_calls;
        if (generation != active_generation ||
            !security_ready_for_hid(generation, connection_handle)) {
            return {.kind = hid_control_executor::GattSchemaStoreResultKind::
                                kStaleIdentity};
        }
        if (gatt_schema_status_result.kind !=
            hid_control_executor::GattSchemaStoreResultKind::kStale) {
            if (gatt_schema_status_result.kind ==
                hid_control_executor::GattSchemaStoreResultKind::kCurrent) {
                gatt_schema_current = true;
            }
            return gatt_schema_status_result;
        }
        if (stored_gatt_schema_current) {
            gatt_schema_current = true;
            return {.kind = hid_control_executor::GattSchemaStoreResultKind::
                                kCurrent};
        }
        return {.kind =
                    hid_control_executor::GattSchemaStoreResultKind::kStale};
    }
    hid_control_executor::GattSchemaStoreResult persist_gatt_schema_current(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) override {
        ++persist_gatt_schema_calls;
        last_gatt_persist_generation = generation;
        last_gatt_persist_connection = connection_handle;
        if (generation != active_generation ||
            !security_ready_for_hid(generation, connection_handle)) {
            return {.kind = hid_control_executor::GattSchemaStoreResultKind::
                                kStaleIdentity};
        }
        if (persist_gatt_schema_result.kind ==
            hid_control_executor::GattSchemaStoreResultKind::kCurrent) {
            stored_gatt_schema_current = true;
            gatt_schema_current = true;
        }
        return persist_gatt_schema_result;
    }
    bool gatt_schema_current_for_hid(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle) const override {
        const auto snapshot = security.snapshot();
        return snapshot.coherent && snapshot.generation == generation &&
               snapshot.connection_handle == connection_handle &&
               gatt_schema_current;
    }
    std::uint16_t service_changed_value_handle() const override {
        return service_changed_handle;
    }
    std::int32_t request_gatt_cache_refresh(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle, std::uint16_t start_handle,
        std::uint16_t end_handle) override {
        ++gatt_cache_refresh_calls;
        last_gatt_refresh_generation = generation;
        last_gatt_refresh_connection = connection_handle;
        last_gatt_refresh_start = start_handle;
        last_gatt_refresh_end = end_handle;
        return gatt_cache_refresh_result;
    }
    hid_control_executor::BleBondListResult list_bonds() override {
        ++bond_list_calls;
        return bond_list_result;
    }
    hid_control_executor::BleBondRemoveResult remove_bond(
        const hid_control_executor::BondId &bond_id) override {
        ++bond_remove_calls;
        last_bond_id = bond_id;
        bond_remove_result.bond_id = bond_id;
        return bond_remove_result;
    }
    void record_heap_checkpoint(HeapCheckpoint checkpoint) override {
        ++heap_checkpoint_calls;
        last_heap_checkpoint = checkpoint;
    }
    bool event(hid_control_executor::BleEventKind kind,
               std::uint16_t connection = ble_lifecycle::kNoConnection,
               std::int32_t status = 0) {
        return event_for_generation(kind, active_generation, connection, status);
    }
    bool event_for_generation(hid_control_executor::BleEventKind kind,
                              ble_lifecycle::Generation generation,
                              std::uint16_t connection = ble_lifecycle::kNoConnection,
                              std::int32_t status = 0) {
        const auto failure_kind =
            kind == hid_control_executor::BleEventKind::kStoreFull
                ? ble_security::StoreFailureKind::kCapacityFull
                : kind == hid_control_executor::BleEventKind::kStorageFailure
                      ? ble_security::StoreFailureKind::kWrite
                      : ble_security::StoreFailureKind::kNone;
        if (failure_kind != ble_security::StoreFailureKind::kNone) {
            (void)security_inhibit.inhibit(
                generation, connection,
                kind == hid_control_executor::BleEventKind::kStorageFailure);
        }
        return sink->signal_ble_event({.kind = kind,
                                      .generation = generation,
                                      .connection_handle = connection,
                                      .status = status,
                                      .store_failure_kind = failure_kind});
    }
    bool lifecycle_event_for_generation(
        hid_control_executor::BleEventKind kind,
        ble_lifecycle::Generation generation, std::int32_t status = 0) {
        assert(kind == hid_control_executor::BleEventKind::kReset ||
               kind == hid_control_executor::BleEventKind::kSync ||
               kind == hid_control_executor::BleEventKind::kTimeout);
        const bool published = event_for_generation(
            kind, generation, ble_lifecycle::kNoConnection, status);
        if (!published) {
            sink->signal_ble_lifecycle_handoff_failure();
        }
        return published;
    }
    hid_control_executor::BleEventSink *sink = nullptr;
    hid_control_executor::BleDatabase *active_database = nullptr;
    ble_lifecycle::Generation active_generation = 0;
    std::uint16_t last_connection = ble_lifecycle::kNoConnection;
    std::uint16_t last_configured_connection = ble_lifecycle::kNoConnection;
    std::uint16_t last_orphan_connection = ble_lifecycle::kNoConnection;
    int initialize_calls = 0;
    int advertising_attempts = 0;
    int advertising_calls = 0;
    int stop_calls = 0;
    int disconnect_calls = 0;
    int arm_route_release_grace_calls = 0;
    int cancel_route_release_grace_calls = 0;
    int orphan_terminate_calls = 0;
    int configure_connection_calls = 0;
    int initiate_security_calls = 0;
    int inject_calls = 0;
    int arm_pairing_timeout_calls = 0;
    int cancel_pairing_timeout_calls = 0;
    int refresh_security_calls = 0;
    int retire_security_calls = 0;
    int apply_store_failure_calls = 0;
    int apply_persistent_store_failure_calls = 0;
    int gatt_schema_status_calls = 0;
    int persist_gatt_schema_calls = 0;
    int gatt_cache_refresh_calls = 0;
    int bond_list_calls = 0;
    int bond_remove_calls = 0;
    int heap_checkpoint_calls = 0;
    std::int32_t initialize_result = 0;
    std::int32_t advertising_result = 0;
    std::int32_t stop_result = 0;
    std::int32_t disconnect_result = 0;
    std::int32_t already_disconnected_result = 7;
    std::int32_t route_release_grace_result = 0;
    std::int32_t orphan_terminate_result = 0;
    std::int32_t configure_connection_result = 0;
    std::int32_t initiate_security_result = 0;
    std::int32_t inject_result = 0;
    std::int32_t gatt_cache_refresh_result = 0;
    std::uint32_t last_injected_value = 0;
    ble_lifecycle::Generation timer_generation = 0;
    std::uint16_t timer_connection = ble_lifecycle::kNoConnection;
    std::uint32_t timer_pairing_id = 0;
    std::uint64_t now_us = 0;
    bool stored_gatt_schema_current = true;
    bool initialize_persistent_failure = false;
    bool gatt_schema_current = false;
    bool adopt_stored_schema_during_security_refresh = true;
    std::uint16_t service_changed_handle = 40;
    ble_lifecycle::Generation last_gatt_refresh_generation = 0;
    ble_lifecycle::Generation last_gatt_persist_generation = 0;
    std::uint16_t last_gatt_refresh_connection =
        ble_lifecycle::kNoConnection;
    std::uint16_t last_gatt_persist_connection =
        ble_lifecycle::kNoConnection;
    std::uint16_t last_gatt_refresh_start = 0;
    std::uint16_t last_gatt_refresh_end = 0;
    hid_control_executor::GattSchemaStoreResult gatt_schema_status_result{
        .kind = hid_control_executor::GattSchemaStoreResultKind::kStale,
    };
    hid_control_executor::GattSchemaStoreResult persist_gatt_schema_result{
        .kind = hid_control_executor::GattSchemaStoreResultKind::kCurrent,
    };
    hid_control_executor::BleBondListResult bond_list_result{
        .kind = hid_control_executor::BleBondListResultKind::kSuccess,
        .healthy = true,
    };
    hid_control_executor::BleBondRemoveResult bond_remove_result{};
    hid_control_executor::BondId last_bond_id{};
    bool route_release_grace_armed = false;
    hid_control_executor::BleRouteReleaseIdentity route_release_identity{};
    ble_security::State security{};
    ble_security::ReadinessInhibit security_inhibit{};
    ble_security::LinkSecurityEvidence security_link{};
    ble_security::PersistedSecurityEvidence security_persisted{};
    HeapCheckpoint last_heap_checkpoint = HeapCheckpoint::kColdBoot;
};

struct FakeBleDatabase final : hid_control_executor::BleDatabase {
    int register_database() override {
        ++register_calls;
        return register_result;
    }
    int validate_registered_database() override {
        ++validate_calls;
        return validate_result;
    }
    void bind_event_sink(hid_control_executor::BleEventSink *value) override {
        sink = value;
    }
    void set_generation(ble_lifecycle::Generation value) override {
        generation = value;
    }
    hid_control_executor::BleHidHandles hid_handles() const override {
        return handles;
    }
    hid_control_executor::BleNotifyBackendResult notify_custom(
        std::uint16_t connection_handle, std::uint16_t characteristic_handle,
        const std::uint8_t *payload, std::uint16_t payload_length) override {
        const std::size_t history_index = static_cast<std::size_t>(notify_calls);
        ++notify_calls;
        last_connection = connection_handle;
        last_characteristic = characteristic_handle;
        last_payload_length = payload_length;
        last_payload.fill(0);
        assert(payload_length <= last_payload.size());
        std::memcpy(last_payload.data(), payload, payload_length);
        if (history_index < notify_history_length.size()) {
            notify_history_connection[history_index] = connection_handle;
            notify_history_characteristic[history_index] = characteristic_handle;
            notify_history_length[history_index] = payload_length;
            notify_history_payload[history_index].fill(0);
            std::memcpy(notify_history_payload[history_index].data(), payload,
                        payload_length);
        }
        return notify_result;
    }
    int register_calls = 0;
    int validate_calls = 0;
    int register_result = 0;
    int validate_result = 0;
    int notify_calls = 0;
    hid_control_executor::BleEventSink *sink = nullptr;
    ble_lifecycle::Generation generation = 0;
    hid_control_executor::BleHidHandles handles{
        .report_map_value = 5,
        .keyboard_value = 10,
        .mouse_value = 20,
        .control_point_value = 30,
    };
    std::uint16_t last_connection = ble_lifecycle::kNoConnection;
    std::uint16_t last_characteristic = 0;
    std::uint16_t last_payload_length = 0;
    std::array<std::uint8_t, 8> last_payload{};
    std::array<std::uint16_t, 64> notify_history_connection{};
    std::array<std::uint16_t, 64> notify_history_characteristic{};
    std::array<std::uint16_t, 64> notify_history_length{};
    std::array<std::array<std::uint8_t, 8>, 64> notify_history_payload{};
    hid_control_executor::BleNotifyBackendResult notify_result =
        hid_control_executor::BleNotifyBackendResult::kStackAccepted;
};

struct AcceptingExecutor final : usb_lifecycle::Executor {
    bool schedule(usb_lifecycle::ExecutorAction, usb_lifecycle::Snapshot) override {
        ++calls;
        return true;
    }
    int calls = 0;
};

struct ReportSink {
    int calls = 0;
    std::uint8_t instance = 0;
    std::uint16_t length = 0;
    std::array<std::uint8_t, 8> report{};

    static bool submit(void *context, std::uint8_t instance,
                       const std::uint8_t *report, std::uint16_t length) {
        auto *sink = static_cast<ReportSink *>(context);
        ++sink->calls;
        sink->instance = instance;
        sink->length = length;
        std::memcpy(sink->report.data(), report, length);
        return true;
    }
};

struct BleSubmitSink {
    hid_runtime::BleSubmitResult result =
        hid_runtime::BleSubmitResult::kStackAccepted;
    int calls = 0;

    static hid_runtime::BleSubmitResult submit(
        void *context, hid_runtime::Interface, hid_runtime::HidWorkToken,
        const std::uint8_t *, std::uint16_t) {
        auto *sink = static_cast<BleSubmitSink *>(context);
        ++sink->calls;
        return sink->result;
    }
};

usb_lifecycle::TransitionResult action(hid_control_executor::CommandOutcome outcome) {
    return outcome.action_result;
}

bool same_lifecycle(const usb_lifecycle::Snapshot &left,
                    const usb_lifecycle::Snapshot &right) {
    return left.desired == right.desired && left.observed == right.observed &&
           left.generation == right.generation &&
           left.uncertainty_generation == right.uncertainty_generation &&
           left.safety_pending == right.safety_pending &&
           left.host_release_uncertain == right.host_release_uncertain &&
           left.recovery_required == right.recovery_required &&
           left.last_error.present == right.last_error.present &&
           left.last_error.operation == right.last_error.operation &&
           left.last_error.code == right.last_error.code;
}

void complete_attach(hid_runtime::Runtime &runtime, FakeBackend &backend,
                     hid_control_executor::Controller &controller) {
    assert(controller.initialize(&runtime, &backend));
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void mount_ready(hid_runtime::Runtime &runtime) {
    runtime.state_machine().on_mount();
    runtime.state_machine().set_ready(hid_runtime::Interface::kKeyboard, true);
    runtime.state_machine().set_ready(hid_runtime::Interface::kMouse, true);
}

void hold_f24(hid_runtime::Runtime &runtime, ReportSink &sink) {
    if (runtime.state_machine().route_snapshot().active == hid_route::OutputRoute::kNone) {
        assert(runtime.state_machine().request_route_usb().action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
    }
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(runtime.state_machine().queue_keyboard_report(0, f24));
    runtime.state_machine().execute(ReportSink::submit, &sink);
    assert(sink.calls == 1 && sink.report[2] == 115);
    assert(runtime.state_machine().report_complete(0));
}

void test_install_and_uninstall_are_task_owned_and_serialized() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));

    const auto accepted_attach = controller.request_attach();
    assert(action(accepted_attach) == usb_lifecycle::TransitionResult::kAccepted);
    assert(accepted_attach.snapshot_valid);
    assert(accepted_attach.snapshot.lifecycle.observed ==
           usb_lifecycle::ObservedState::kAttaching);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(backend.install_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);

    mount_ready(runtime);
    const auto old_generation = controller.snapshot().lifecycle.generation;
    const auto accepted_detach = controller.request_detach();
    assert(action(accepted_detach) == usb_lifecycle::TransitionResult::kAccepted);
    assert(accepted_detach.snapshot_valid);
    assert(accepted_detach.snapshot.lifecycle.observed ==
           usb_lifecycle::ObservedState::kDetaching);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbDetach);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(backend.uninstall_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.uninstall_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    const auto hidden = controller.snapshot();
    assert(hidden.lifecycle.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(hidden.lifecycle.generation == old_generation + 1);
    assert(!hidden.lifecycle.recovery_required);
}

void test_route_owner_blocks_attach_before_lifecycle_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    const auto before = controller.snapshot().lifecycle;

    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(before, controller.snapshot().lifecycle));
    assert(backend.install_calls == 0);
    assert(!controller.process_one_for_test());
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_route_owner_blocks_detach_before_lifecycle_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    const auto before = controller.snapshot().lifecycle;

    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(before, controller.snapshot().lifecycle));
    assert(backend.uninstall_calls == 0);
    assert(!controller.process_one_for_test());
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_usb_owner_blocks_route_reservation() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
        assert(!controller.reserve_operation_for_test(ControlOperation::kRouteChange));
        assert(controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        complete_attach(runtime, backend, controller);
        mount_ready(runtime);
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.active_operation_for_test() == ControlOperation::kUsbDetach);
        assert(!controller.reserve_operation_for_test(ControlOperation::kRouteChange));
        assert(controller.process_one_for_test());
    }
}

void test_no_op_and_ordinary_rejection_release_guard() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kNoOp);
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        assert(!controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        complete_attach(runtime, backend, controller);
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kNoOp);
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        assert(!controller.process_one_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        AcceptingExecutor external;
        assert(controller.initialize(&runtime, &backend));
        assert(runtime.state_machine().request_usb_attach(external).action_result ==
               usb_lifecycle::TransitionResult::kAccepted);
        const auto before = controller.snapshot().lifecycle;
        assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
        assert(same_lifecycle(before, controller.snapshot().lifecycle));
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
    }
}

void test_schedule_requires_matching_preclaimed_owner() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    const auto snapshot = controller.snapshot().lifecycle;

    assert(!controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(controller.reserve_operation_for_test(ControlOperation::kRouteChange));
    assert(!controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    controller.release_operation_for_test(ControlOperation::kRouteChange);
    assert(controller.reserve_operation_for_test(ControlOperation::kUsbAttach));
    assert(controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_backend_failures_release_guard() {
    for (const auto kind : {hid_control_executor::BackendResultKind::kCleanInstallFailure,
                            hid_control_executor::BackendResultKind::kAmbiguousInstallFailure}) {
        hid_runtime::Runtime runtime;
        FakeBackend backend;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &backend));
        backend.install_result = {.kind = kind, .error_code = -99};
        assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        const auto snapshot = controller.snapshot().lifecycle;
        assert(snapshot.last_error.present && snapshot.last_error.code == -99);
        assert(snapshot.recovery_required ==
               (kind == hid_control_executor::BackendResultKind::kAmbiguousInstallFailure));
    }

    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    backend.uninstall_result = {
        .kind = hid_control_executor::BackendResultKind::kUninstallFailure,
        .error_code = -77,
    };
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    const auto failed = controller.snapshot().lifecycle;
    assert(failed.recovery_required);
    assert(failed.last_error.present && failed.last_error.code == -77);
}

void test_real_queue_failure_is_recovery_fault_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    controller.fail_next_enqueue_for_test();

    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    const auto failed = controller.snapshot().lifecycle;
    assert(failed.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(failed.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(failed.generation == 1);
    assert(failed.recovery_required);
    assert(failed.last_error.present && failed.last_error.code == -1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_callback_invalidation_obsoletes_action_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    const auto authority_before = runtime.state_machine().authority_epoch();
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    const auto accepted_generation = controller.snapshot().lifecycle.generation;
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);

    runtime.state_machine().on_unmount();
    assert(runtime.state_machine().authority_epoch() != authority_before);
    assert(controller.snapshot().lifecycle.generation != accepted_generation);
    assert(controller.active_operation_for_test() == ControlOperation::kUsbAttach);
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 0);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_route_zero_work_is_synchronous_and_never_uninstalls_usb() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    const auto usb_generation = controller.snapshot().lifecycle.generation;

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(release.snapshot_valid);
    assert(release.snapshot.route.desired == hid_route::OutputRoute::kNone);
    assert(release.snapshot.route.active == hid_route::OutputRoute::kUsb);
    assert(release.snapshot.route.transition == hid_route::Transition::kReleasing);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
    assert(backend.uninstall_calls == 0);
    assert(controller.snapshot().lifecycle.generation == usb_generation);
    assert(controller.snapshot().lifecycle.observed ==
           usb_lifecycle::ObservedState::kMounted);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);

    const auto select = controller.request_route(hid_route::OutputRoute::kUsb);
    assert(select.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(select.snapshot.ready);
    assert(backend.install_calls == 1);
    assert(controller.snapshot().lifecycle.generation == usb_generation);
}

void test_route_release_owns_guard_and_blocks_detach_before_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    const auto lifecycle_before = controller.snapshot().lifecycle;

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kRouteChange);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(same_lifecycle(lifecycle_before, controller.snapshot().lifecycle));
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.snapshot().lifecycle.host_release_uncertain);
    assert(backend.uninstall_calls == 0);
    assert(!controller.process_one_for_test());
}

void test_usb_detach_owner_blocks_route_without_duplicate_action() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    const auto route = controller.request_route(hid_route::OutputRoute::kNone);
    assert(route.action_result == hid_runtime::RouteTransitionResult::kBusy);
    assert(controller.process_one_for_test());
    assert(backend.uninstall_calls == 1);
    assert(!controller.process_one_for_test());
}

void test_route_schedule_failure_terminalizes_none_and_releases_guard() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    controller.fail_next_enqueue_for_test();

    const auto release = controller.request_route(hid_route::OutputRoute::kNone);
    assert(release.action_result == hid_runtime::RouteTransitionResult::kBusy);
    assert(!release.snapshot_valid);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.snapshot().lifecycle.host_release_uncertain);
    assert(!controller.process_one_for_test());
}

void test_unmount_preempts_route_action_and_guard_releases_once() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    hid_control_executor::Controller controller;
    ReportSink sink;
    complete_attach(runtime, backend, controller);
    mount_ready(runtime);
    hold_f24(runtime, sink);
    assert(controller.request_route(hid_route::OutputRoute::kNone).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(controller.active_operation_for_test() == ControlOperation::kRouteChange);

    runtime.state_machine().on_unmount();
    assert(controller.route_snapshot().route.active == hid_route::OutputRoute::kNone);
    assert(controller.process_one_for_test());
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.process_one_for_test());
}

void test_ble_lifecycle_is_shared_serialized_and_transport_independent() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(ble.heap_checkpoint_calls == 1);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kColdBoot);
    const auto route_before = controller.route_snapshot();
    const auto epoch_before = runtime.state_machine().authority_epoch();

    const auto enable = controller.request_ble_enable();
    assert(enable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    assert(enable.snapshot.observed == ble_lifecycle::ObservedState::kEnabling);
    assert(controller.active_operation_for_test() == ControlOperation::kBleEnable);
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kBusy);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kBusy);
    assert(controller.process_one_for_test());
    assert(ble.initialize_calls == 1);
    assert(database.register_calls == 1);
    assert(database.validate_calls == 0);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kBeforeFirstEnable);
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kAdvertising);
    assert(database.validate_calls == 1);
    assert(ble.advertising_attempts == 1);
    assert(ble.advertising_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.route_snapshot().route.generation ==
           route_before.route.generation);
    assert(runtime.state_machine().authority_epoch() == epoch_before);

    assert(ble.event(hid_control_executor::BleEventKind::kConnect, 42));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
    assert(ble.configure_connection_calls == 1);
    assert(ble.last_configured_connection == 42);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kConnected);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 42));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().advertising);
    assert(controller.ble_snapshot().generation == 2);
    assert(ble.advertising_calls == 2);
    assert(ble.last_heap_checkpoint ==
           hid_control_executor::BleBackend::HeapCheckpoint::kReadvertising);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(runtime.state_machine().authority_epoch() == epoch_before);
}

void test_ble_disable_expected_disconnect_and_retained_stack() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kConnect, 11));
    assert(controller.process_one_for_test());

    const auto disable = controller.request_ble_disable();
    assert(disable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    const auto disable_generation = disable.snapshot.generation;
    assert(controller.process_one_for_test());
    assert(ble.disconnect_calls == 1 && ble.last_connection == 11);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 11));
    assert(controller.process_one_for_test());
    const auto idle = controller.ble_snapshot();
    assert(idle.generation == disable_generation);
    assert(idle.observed == ble_lifecycle::ObservedState::kIdle);
    assert(idle.stack_ready && !idle.advertising && !idle.connected);
    assert(ble.initialize_calls == 1);
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kNoOp);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.initialize_calls == 1);
    assert(database.register_calls == 1);
    assert(database.validate_calls == 2);
}

void test_missing_live_database_fails_closed_before_advertising() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    database.validate_result = 404;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(database.register_calls == 1);
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());

    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required);
    assert(!failed.advertising && !failed.connected);
    assert(failed.last_error.present);
    assert(failed.last_error.operation == ble_lifecycle::Operation::kEnable);
    assert(failed.last_error.code == 404);
    assert(database.validate_calls == 1);
    assert(ble.advertising_attempts == 1);
    assert(ble.advertising_calls == 0);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
}

void test_stale_sync_cannot_validate_or_advertise() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    const auto enable = controller.request_ble_enable();
    assert(enable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    assert(enable.snapshot.generation == 1);
    assert(controller.process_one_for_test());

    assert(ble.event_for_generation(hid_control_executor::BleEventKind::kSync, 0));
    assert(controller.process_one_for_test());
    assert(database.validate_calls == 0);
    assert(ble.advertising_attempts == 0);
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kEnabling);

    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(database.validate_calls == 1);
    assert(ble.advertising_calls == 1);
}

void test_retained_cycles_validate_without_duplicate_registration() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));

    for (int cycle = 0; cycle < 3; ++cycle) {
        assert(controller.request_ble_enable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        if (cycle == 0) {
            assert(ble.event(hid_control_executor::BleEventKind::kSync));
            assert(controller.process_one_for_test());
        }
        assert(controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kAdvertising);
        assert(controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        assert(controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kIdle);
    }
    assert(ble.initialize_calls == 1);
    assert(database.register_calls == 1);
    assert(database.validate_calls == 3);
    assert(ble.advertising_attempts == 3);
    assert(ble.advertising_calls == 3);
}

void test_ble_busy_has_no_stage_a_and_usb_detach_does_not_change_ble() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    const auto cold = controller.ble_snapshot();
    assert(controller.reserve_operation_for_test(ControlOperation::kUsbDetach));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
    assert(controller.ble_snapshot().generation == cold.generation);
    assert(controller.ble_snapshot().desired == cold.desired);
    controller.release_operation_for_test(ControlOperation::kUsbDetach);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    const auto exposed = controller.ble_snapshot();
    assert(action(controller.request_attach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    runtime.state_machine().on_mount();
    assert(action(controller.request_detach()) == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().desired == exposed.desired);
    assert(controller.ble_snapshot().generation == exposed.generation);
    assert(controller.ble_snapshot().advertising);
}

void advertise_ble(hid_runtime::Runtime &runtime, FakeBackend &usb,
                   FakeBleBackend &ble, FakeBleDatabase &database,
                   hid_control_executor::Controller &controller) {
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kAdvertising);
}

void connect_ble(hid_runtime::Runtime &runtime, FakeBackend &usb,
                 FakeBleBackend &ble, FakeBleDatabase &database,
                 hid_control_executor::Controller &controller,
                 std::uint16_t handle = 23) {
    advertise_ble(runtime, usb, ble, database, controller);
    assert(ble.event(hid_control_executor::BleEventKind::kConnect, handle));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
}

void hide_ble(hid_runtime::Runtime &runtime, FakeBackend &usb,
              FakeBleBackend &ble, FakeBleDatabase &database,
              hid_control_executor::Controller &controller) {
    advertise_ble(runtime, usb, ble, database, controller);
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto hidden = controller.ble_snapshot();
    assert(hidden.stack_ready && !hidden.advertising && !hidden.connected);
    assert(hidden.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(hidden.observed == ble_lifecycle::ObservedState::kIdle);
}

hid_control_executor::BondId executor_bond_id(char digit) {
    hid_control_executor::BondId result{};
    result.fill(digit);
    result.back() = '\0';
    return result;
}

void test_ble_disable_preserves_active_usb_route_and_reports() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    ReportSink usb_sink;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    complete_attach(runtime, usb, controller);
    mount_ready(runtime);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);

    const std::array<std::uint8_t, 6> held_keys{0x73, 0, 0, 0, 0, 0};
    assert(runtime.state_machine().queue_keyboard_report(0, held_keys));
    runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 1 && usb_sink.instance == 0 &&
           usb_sink.length == 8 && usb_sink.report[2] == 0x73);
    assert(runtime.state_machine().report_complete(0));
    assert(runtime.state_machine().queue_mouse_report(1, 0, 0, 0, 0));
    runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 2 && usb_sink.instance == 1 &&
           usb_sink.length == 5 && usb_sink.report[0] == 1);
    assert(runtime.state_machine().report_complete(1));

    connect_ble(runtime, usb, ble, database, controller, 70);
    const auto route_before = runtime.state_machine().route_snapshot();
    const auto authority_before = runtime.state_machine().authority_epoch();
    const auto attach_generation_before =
        runtime.state_machine().attach_generation();
    const auto keyboard_before = runtime.state_machine().keyboard_state();
    const auto mouse_before = runtime.state_machine().mouse_state();
    assert(route_before.desired == hid_route::OutputRoute::kUsb);
    assert(route_before.active == hid_route::OutputRoute::kUsb);
    assert(route_before.transition == hid_route::Transition::kStable);

    const auto disable = controller.request_ble_disable();
    assert(disable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    assert(disable.snapshot.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(disable.snapshot.observed == ble_lifecycle::ObservedState::kDisabling);
    assert(controller.process_one_for_test());
    assert(ble.disconnect_calls == 1 && ble.last_connection == 70);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 70));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kIdle);

    const auto route_after = runtime.state_machine().route_snapshot();
    assert(route_after.desired == route_before.desired);
    assert(route_after.active == route_before.active);
    assert(route_after.transition == route_before.transition);
    assert(route_after.generation == route_before.generation);
    assert(runtime.state_machine().authority_epoch() == authority_before);
    assert(runtime.state_machine().attach_generation() ==
           attach_generation_before);
    assert(runtime.state_machine().keyboard_state().modifiers ==
           keyboard_before.modifiers);
    assert(runtime.state_machine().keyboard_state().keycodes ==
           keyboard_before.keycodes);
    assert(runtime.state_machine().mouse_state().buttons ==
           mouse_before.buttons);
    assert(!runtime.state_machine().safety_required(
        hid_runtime::Interface::kKeyboard));
    assert(!runtime.state_machine().safety_required(
        hid_runtime::Interface::kMouse));
    assert(usb_sink.calls == 2);
    assert(database.notify_calls == 0);

    const std::array<std::uint8_t, 6> next_keys{0x72, 0, 0, 0, 0, 0};
    assert(runtime.state_machine().queue_keyboard_report(2, next_keys));
    runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 3 && usb_sink.instance == 0 &&
           usb_sink.length == 8 && usb_sink.report[0] == 2 &&
           usb_sink.report[2] == 0x72);
    assert(runtime.state_machine().report_complete(0));
    assert(runtime.state_machine().queue_mouse_report(3, 1, -1, 2, -2));
    runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 4 && usb_sink.instance == 1 &&
           usb_sink.length == 5 && usb_sink.report[0] == 3 &&
           usb_sink.report[1] == 1 && usb_sink.report[2] == 0xff &&
           usb_sink.report[3] == 2 && usb_sink.report[4] == 0xfe);
    assert(runtime.state_machine().report_complete(1));
    assert(database.notify_calls == 0);
}

void test_ble_disable_failure_preserves_active_usb_route() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    ReportSink usb_sink;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    complete_attach(runtime, usb, controller);
    mount_ready(runtime);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    advertise_ble(runtime, usb, ble, database, controller);
    const auto route_before = runtime.state_machine().route_snapshot();
    const auto authority_before = runtime.state_machine().authority_epoch();
    ble.stop_result = -70;

    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto failure = controller.ble_snapshot();
    assert(failure.observed == ble_lifecycle::ObservedState::kFault);
    assert(failure.recovery_required);
    const auto route_after = runtime.state_machine().route_snapshot();
    assert(route_after.desired == route_before.desired);
    assert(route_after.active == route_before.active);
    assert(route_after.transition == route_before.transition);
    assert(route_after.generation == route_before.generation);
    assert(runtime.state_machine().authority_epoch() == authority_before);
    assert(!runtime.state_machine().safety_required(
        hid_runtime::Interface::kKeyboard));
    assert(!runtime.state_machine().safety_required(
        hid_runtime::Interface::kMouse));

    const std::array<std::uint8_t, 6> keys{0x71, 0, 0, 0, 0, 0};
    assert(runtime.state_machine().queue_keyboard_report(0, keys));
    runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 1 && usb_sink.instance == 0 &&
           usb_sink.report[2] == 0x71);
    assert(database.notify_calls == 0);
}

void test_usb_route_release_serializes_ble_disable_without_ble_stage_a() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    ReportSink usb_sink;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    complete_attach(runtime, usb, controller);
    mount_ready(runtime);
    assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    hold_f24(runtime, usb_sink);
    advertise_ble(runtime, usb, ble, database, controller);

    assert(controller.request_route(hid_route::OutputRoute::kNone).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    const auto releasing = runtime.state_machine().route_snapshot();
    assert(releasing.active == hid_route::OutputRoute::kUsb);
    assert(releasing.transition == hid_route::Transition::kReleasing);
    assert(controller.active_operation_for_test() == ControlOperation::kRouteChange);
    const auto ble_before = controller.ble_snapshot();
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
    const auto ble_after = controller.ble_snapshot();
    assert(ble_after.desired == ble_before.desired);
    assert(ble_after.observed == ble_before.observed);
    assert(ble_after.generation == ble_before.generation);

    assert(controller.process_one_for_test());
    const auto none = runtime.state_machine().route_snapshot();
    assert(none.active == hid_route::OutputRoute::kNone);
    assert(none.transition == hid_route::Transition::kStable);
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kIdle);
}

ble_security::StoredSecurityRecord valid_security_record(bool sc = true) {
    return {.found = true,
            .identity_matches = true,
            .ltk_present = true,
            .authenticated = true,
            .secure_connections = sc,
            .key_size = ble_security::kRequiredKeySize};
}

void make_security_ready(FakeBleBackend &ble, bool identity = true) {
    ble.security_link = {.encrypted = true,
                         .authenticated = true,
                         .nimble_bonded = true,
                         .secure_connections = true,
                         .identity_resolved = identity,
                         .key_size = ble_security::kRequiredKeySize};
    ble.security_persisted = {.our = valid_security_record(),
                              .peer = valid_security_record()};
}

bool queue_subscription(
    hid_control_executor::Controller &controller,
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    hid_control_executor::BleHidInterface interface,
    std::uint16_t attribute_handle, bool enabled,
    hid_control_executor::BleSubscriptionReason reason =
        hid_control_executor::BleSubscriptionReason::kWrite) {
    return controller.signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kSubscription,
        .generation = generation,
        .connection_handle = connection_handle,
        .attribute_handle = attribute_handle,
        .hid_interface = interface,
        .subscription_reason = reason,
        .notify_enabled = enabled,
    });
}

bool queue_control_point(hid_control_executor::Controller &controller,
                         ble_lifecycle::Generation generation,
                         std::uint16_t connection_handle,
                         std::uint16_t attribute_handle, bool suspended) {
    return controller.signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kControlPoint,
        .generation = generation,
        .connection_handle = connection_handle,
        .attribute_handle = attribute_handle,
        .suspended = suspended,
    });
}

bool queue_report_map_read(
    hid_control_executor::Controller &controller,
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint16_t attribute_handle) {
    return controller.signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kReportMapRead,
        .generation = generation,
        .connection_handle = connection_handle,
        .attribute_handle = attribute_handle,
    });
}

bool queue_service_changed_subscription(
    hid_control_executor::Controller &controller,
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint16_t attribute_handle, bool enabled,
    hid_control_executor::BleSubscriptionReason reason =
        hid_control_executor::BleSubscriptionReason::kRestore) {
    return controller.signal_ble_event({
        .kind =
            hid_control_executor::BleEventKind::kServiceChangedSubscription,
        .generation = generation,
        .connection_handle = connection_handle,
        .attribute_handle = attribute_handle,
        .subscription_reason = reason,
        .indicate_enabled = enabled,
    });
}

void subscribe_composite(hid_control_executor::Controller &controller,
                         FakeBleDatabase &database,
                         ble_lifecycle::Generation generation,
                         std::uint16_t connection_handle,
                         hid_control_executor::BleSubscriptionReason reason =
                             hid_control_executor::BleSubscriptionReason::kWrite) {
    assert(queue_subscription(controller, generation, connection_handle,
                              hid_control_executor::BleHidInterface::kKeyboard,
                              database.handles.keyboard_value, true, reason));
    assert(controller.process_one_for_test());
    assert(queue_subscription(controller, generation, connection_handle,
                              hid_control_executor::BleHidInterface::kMouse,
                              database.handles.mouse_value, true, reason));
    assert(controller.process_one_for_test());
}

void fill_queue_with_stale_security_events(
    FakeBleBackend &ble, ble_lifecycle::Generation generation,
    std::uint16_t connection, std::size_t count);

void test_generation_owned_cccd_restore_term_and_clearing() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 71;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    auto peer = controller.ble_hid_peer_snapshot();
    assert(peer.active && peer.generation == generation);
    assert(peer.connection_handle == connection && !peer.suspended);
    assert(!peer.keyboard_notify_enabled && !peer.mouse_notify_enabled);

    // A restored CCCD is retained independently of security event ordering,
    // but cannot make the composite link ready on its own.
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, true,
        hid_control_executor::BleSubscriptionReason::kRestore));
    assert(controller.process_one_for_test());
    assert(controller.ble_hid_peer_snapshot().keyboard_notify_enabled);
    assert(!controller.ble_link_ready());
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kMouse,
        database.handles.mouse_value, true,
        hid_control_executor::BleSubscriptionReason::kRestore));
    assert(controller.process_one_for_test());
    assert(!controller.ble_link_ready());

    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     connection));
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());

    // Unknown, mismatched, and stale tuples cannot mutate current evidence.
    assert(queue_subscription(
        controller, generation - 1U, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(controller.process_one_for_test());
    assert(queue_subscription(
        controller, generation, static_cast<std::uint16_t>(connection + 1U),
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(controller.process_one_for_test());
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard, 999, false));
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());

    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kTimeout, generation - 1U,
        ble_lifecycle::kNoConnection, -9));
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());

    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false,
        hid_control_executor::BleSubscriptionReason::kTerm));
    assert(controller.process_one_for_test());
    assert(!controller.ble_link_ready());
    assert(!controller.ble_hid_peer_snapshot().keyboard_notify_enabled);

    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, true,
        hid_control_executor::BleSubscriptionReason::kRestore));
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kMouse,
        database.handles.mouse_value, false));
    assert(controller.process_one_for_test());
    assert(!controller.ble_link_ready());

    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     connection));
    assert(controller.process_one_for_test());
    assert(!controller.ble_hid_peer_snapshot().active);
    const auto next_generation = controller.ble_snapshot().generation;
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     static_cast<std::uint16_t>(connection + 1U)));
    assert(controller.process_one_for_test());
    const auto next_peer = controller.ble_hid_peer_snapshot();
    assert(next_peer.active && next_peer.generation == next_generation);
    assert(!next_peer.suspended && !next_peer.keyboard_notify_enabled &&
           !next_peer.mouse_notify_enabled);
}

void test_ble_link_readiness_security_and_suspend_predicate() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 72;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    assert(!controller.ble_link_ready());

    ble.security_link.encrypted = true;
    ble.refresh_security(connection);
    assert(!controller.ble_link_ready());
    ble.security_link.authenticated = true;
    ble.refresh_security(connection);
    assert(!controller.ble_link_ready());
    ble.security_link.key_size = 15;
    ble.refresh_security(connection);
    assert(!controller.ble_link_ready());
    ble.security_link.key_size = ble_security::kRequiredKeySize;
    ble.refresh_security(connection);
    assert(!controller.ble_link_ready());
    ble.security_link.nimble_bonded = true;
    ble.security_link.identity_resolved = true;
    ble.refresh_security(connection);
    assert(!controller.ble_link_ready());
    ble.security_persisted = {.our = valid_security_record(),
                              .peer = valid_security_record()};
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    assert(queue_control_point(controller, generation, connection,
                               database.handles.control_point_value, true));
    assert(controller.process_one_for_test());
    assert(controller.ble_hid_peer_snapshot().suspended);
    assert(!controller.ble_link_ready());
    assert(queue_control_point(controller, generation - 1U, connection,
                               database.handles.control_point_value, false));
    assert(controller.process_one_for_test());
    assert(controller.ble_hid_peer_snapshot().suspended);
    assert(queue_control_point(controller, generation, connection,
                               database.handles.control_point_value, false));
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());

    ble.apply_persistent_store_failure(
        ble_security::StoreFailureKind::kWrite, -1);
    assert(!controller.ble_link_ready());

    const auto disable = controller.request_ble_disable();
    assert(disable.action_result == ble_lifecycle::TransitionResult::kAccepted);
    assert(!controller.ble_link_ready());
    assert(controller.process_one_for_test());
    assert(!controller.ble_hid_peer_snapshot().active);
}

void test_gatt_cache_legacy_refresh_read_persist_and_current_reconnect() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 140;
    database.handles = {
        .report_map_value =
            ble_hid_service::kRevision1ReportMapValueHandle,
        .keyboard_value = ble_hid_service::kRevision1KeyboardValueHandle,
        .mouse_value = ble_hid_service::kRevision1MouseValueHandle,
        .control_point_value =
            ble_hid_service::kRevision1ControlPointValueHandle,
    };
    ble.stored_gatt_schema_current = false;  // Legacy record: key absent.
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    // NimBLE v5.5.4 emits RESTORE even for stored CCCD value handles that are
    // absent from the current configurable-characteristic table.  Exact
    // current-handle fencing prevents both legacy HID handles from becoming
    // current subscription evidence.
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        ble_hid_service::kLegacyKeyboardValueHandle, true,
        hid_control_executor::BleSubscriptionReason::kRestore));
    assert(controller.process_one_for_test());
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kMouse,
        ble_hid_service::kLegacyMouseValueHandle, true,
        hid_control_executor::BleSubscriptionReason::kRestore));
    assert(controller.process_one_for_test());
    auto peer = controller.ble_hid_peer_snapshot();
    assert(!peer.keyboard_notify_enabled && !peer.mouse_notify_enabled);

    subscribe_composite(controller, database, generation, connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     connection));
    assert(controller.process_one_for_test());

    // Cache 1/2: legacy security records remain valid, but CCCDs and complete
    // security evidence cannot bypass the missing schema revision.
    assert(ble.security_ready_for_hid(generation, connection));
    assert(ble.gatt_schema_status_calls == 1);
    assert(!ble.gatt_schema_current);
    assert(!controller.ble_link_ready());

    // Cache 3 / API seam: restored Service Changed subscription permits one
    // conservative full-database request for this exact connection.
    assert(queue_service_changed_subscription(
        controller, generation, connection, ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 1);
    assert(ble.last_gatt_refresh_generation == generation);
    assert(ble.last_gatt_refresh_connection == connection);
    assert(ble.last_gatt_refresh_start ==
           hid_control_executor::kGattChangedStartHandle);
    assert(ble.last_gatt_refresh_end ==
           hid_control_executor::kGattChangedEndHandle);
    assert(queue_service_changed_subscription(
        controller, generation, connection, ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 1);
    assert(!controller.ble_link_ready());

    // Cache 4/5: only an exact Report Map read persists current revision for
    // the exact generation and connection, then restores route eligibility.
    assert(queue_report_map_read(controller, generation, connection,
                                 database.handles.report_map_value));
    assert(controller.process_one_for_test());
    assert(ble.persist_gatt_schema_calls == 1);
    assert(ble.last_gatt_persist_generation == generation);
    assert(ble.last_gatt_persist_connection == connection);
    assert(ble.stored_gatt_schema_current && ble.gatt_schema_current);
    assert(controller.ble_link_ready());
    assert(runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);

    // Cache 6: reconnect reads the durable current revision and does not ask
    // for another Service Changed indication.
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     connection));
    assert(controller.process_one_for_test());
    const auto next_generation = controller.ble_snapshot().generation;
    constexpr std::uint16_t next_connection = 141;
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     next_connection));
    assert(controller.process_one_for_test());
    subscribe_composite(controller, database, next_generation, next_connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(ble);
    ble.adopt_stored_schema_during_security_refresh = false;
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     next_connection));
    assert(controller.process_one_for_test());
    assert(ble.gatt_schema_status_calls == 2);
    assert(controller.ble_link_ready());
    assert(queue_service_changed_subscription(
        controller, next_generation, next_connection,
        ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 1);
}

void test_gatt_cache_non_authoritative_refresh_results_and_independent_read() {
    // Cache 7 / API failure semantics: request acceptance, failure, timeout,
    // or confirmation are deliberately not modeled as cache-current evidence.
    // With no Report Map read, every case stays stale and attempts only once.
    for (const std::int32_t request_result : {0, -7, -8}) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        const auto connection = static_cast<std::uint16_t>(142 - request_result);
        ble.stored_gatt_schema_current = false;
        ble.gatt_cache_refresh_result = request_result;
        connect_ble(runtime, usb, ble, database, controller, connection);
        const auto generation = controller.ble_snapshot().generation;
        subscribe_composite(controller, database, generation, connection);
        make_security_ready(ble);
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection));
        assert(controller.process_one_for_test());
        assert(queue_service_changed_subscription(
            controller, generation, connection, ble.service_changed_handle,
            true));
        assert(controller.process_one_for_test());
        assert(ble.gatt_cache_refresh_calls == 1);
        assert(!ble.stored_gatt_schema_current && !ble.gatt_schema_current);
        assert(ble.persist_gatt_schema_calls == 0);
        assert(!controller.ble_link_ready());
        assert(queue_service_changed_subscription(
            controller, generation, connection, ble.service_changed_handle,
            true));
        assert(controller.process_one_for_test());
        assert(ble.gatt_cache_refresh_calls == 1);
    }

    // Cache 8/14: fresh discovery can read Report Map before identity settles;
    // the executor retains only connection-local evidence and persists it once
    // that exact bond becomes authenticated and identity-qualified.
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 151;
    ble.stored_gatt_schema_current = false;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    assert(queue_report_map_read(controller, generation, connection,
                                 database.handles.report_map_value));
    assert(controller.process_one_for_test());
    assert(ble.persist_gatt_schema_calls == 0);
    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete,
                     connection));
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     connection));
    assert(controller.process_one_for_test());
    assert(ble.persist_gatt_schema_calls == 1);
    assert(ble.stored_gatt_schema_current);
    assert(ble.gatt_cache_refresh_calls == 0);
}

void test_gatt_cache_identity_disconnect_and_subscription_fencing() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t reused_connection = 152;
    ble.stored_gatt_schema_current = false;
    connect_ble(runtime, usb, ble, database, controller, reused_connection);
    const auto generation_a = controller.ble_snapshot().generation;
    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     reused_connection));
    assert(controller.process_one_for_test());

    // Cache 12: no Service Changed subscription means no request, no false
    // readiness, and no retry loop.
    assert(ble.gatt_cache_refresh_calls == 0);
    assert(!controller.ble_link_ready());
    assert(queue_service_changed_subscription(
        controller, generation_a, reused_connection,
        ble.service_changed_handle, false));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 0);

    // Cache 10: disconnect before Report Map consumption never persists.
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     reused_connection));
    assert(controller.process_one_for_test());
    assert(!ble.stored_gatt_schema_current);
    assert(ble.persist_gatt_schema_calls == 0);

    // Cache 9: a reused connection handle is fenced by generation; old reads,
    // wrong handles, and wrong connections cannot update the new peer.
    const auto generation_b = controller.ble_snapshot().generation;
    assert(generation_b != generation_a);
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     reused_connection));
    assert(controller.process_one_for_test());
    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     reused_connection));
    assert(controller.process_one_for_test());
    assert(queue_report_map_read(controller, generation_a, reused_connection,
                                 database.handles.report_map_value));
    assert(controller.process_one_for_test());
    assert(queue_report_map_read(controller, generation_b, reused_connection,
                                 database.handles.keyboard_value));
    assert(controller.process_one_for_test());
    assert(queue_report_map_read(controller, generation_b,
                                 reused_connection + 1U,
                                 database.handles.report_map_value));
    assert(controller.process_one_for_test());
    assert(ble.persist_gatt_schema_calls == 0);
    assert(!ble.stored_gatt_schema_current);

    // The still-stale reconnect gets one new bounded attempt when its restored
    // Service Changed subscription is observed.
    assert(queue_service_changed_subscription(
        controller, generation_a, reused_connection,
        ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(queue_service_changed_subscription(
        controller, generation_b, reused_connection,
        static_cast<std::uint16_t>(ble.service_changed_handle + 1U), true));
    assert(controller.process_one_for_test());
    assert(queue_service_changed_subscription(
        controller, generation_b, reused_connection + 1U,
        ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 0);
    assert(queue_service_changed_subscription(
        controller, generation_b, reused_connection,
        ble.service_changed_handle, true));
    assert(controller.process_one_for_test());
    assert(ble.gatt_cache_refresh_calls == 1);
}

void test_gatt_cache_persistence_failures_follow_store_policy() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::uint16_t connection = 153;
        ble.stored_gatt_schema_current = false;
        ble.gatt_schema_status_result = {
            .kind = hid_control_executor::GattSchemaStoreResultKind::
                kStorageFailure,
            .status = -30,
        };
        connect_ble(runtime, usb, ble, database, controller, connection);
        make_security_ready(ble);
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection));
        assert(controller.process_one_for_test());
        assert(ble.apply_persistent_store_failure_calls == 1);
        assert(controller.ble_snapshot().recovery_required);
        assert(!controller.ble_link_ready());
        assert(ble.persist_gatt_schema_calls == 0);
    }
    for (const auto kind : {
             hid_control_executor::GattSchemaStoreResultKind::kCapacityFull,
             hid_control_executor::GattSchemaStoreResultKind::kStorageFailure}) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::uint16_t connection = 154;
        ble.stored_gatt_schema_current = false;
        ble.persist_gatt_schema_result = {.kind = kind, .status = -31};
        connect_ble(runtime, usb, ble, database, controller, connection);
        const auto generation = controller.ble_snapshot().generation;
        make_security_ready(ble);
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection));
        assert(controller.process_one_for_test());
        assert(queue_report_map_read(controller, generation, connection,
                                     database.handles.report_map_value));
        assert(controller.process_one_for_test());
        assert(!ble.stored_gatt_schema_current && !ble.gatt_schema_current);
        assert(!controller.ble_link_ready());
        if (kind == hid_control_executor::GattSchemaStoreResultKind::
                        kCapacityFull) {
            assert(ble.apply_store_failure_calls == 1);
            assert(ble.apply_persistent_store_failure_calls == 0);
            assert(!controller.ble_snapshot().recovery_required);
            assert(ble.disconnect_calls == 1);
        } else {
            assert(ble.apply_store_failure_calls == 0);
            assert(ble.apply_persistent_store_failure_calls == 1);
            assert(controller.ble_snapshot().recovery_required);
        }
    }
}

void test_gatt_cache_dropped_report_map_read_fails_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 155;
    ble.stored_gatt_schema_current = false;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    make_security_ready(ble);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     connection));
    assert(controller.process_one_for_test());
    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);

    // Cache 13: callback evidence that cannot enter the bounded queue is never
    // converted into durable current state; the existing overflow path wins.
    assert(!queue_report_map_read(controller, generation, connection,
                                  database.handles.report_map_value));
    assert(ble.persist_gatt_schema_calls == 0);
    assert(!ble.stored_gatt_schema_current && !ble.gatt_schema_current);
    assert(controller.process_wake_cycle_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_link_ready());
    assert(ble.persist_gatt_schema_calls == 0);
}

void test_store_failure_immediately_inhibits_stale_verification() {
    for (const auto kind : {hid_control_executor::BleEventKind::kStoreFull,
                            hid_control_executor::BleEventKind::kStorageFailure}) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::uint16_t connection = 74;
        connect_ble(runtime, usb, ble, database, controller, connection);
        const auto generation = controller.ble_snapshot().generation;
        subscribe_composite(controller, database, generation, connection);
        make_security_ready(ble);
        ble.refresh_security(connection);
        assert(controller.ble_link_ready());

        const int local_applies_before = ble.apply_store_failure_calls;
        const int global_applies_before =
            ble.apply_persistent_store_failure_calls;
        assert(ble.event(kind, connection, -17));
        // Callback capture inhibits readiness before the queued event runs.
        assert(!controller.ble_link_ready());
        assert(ble.apply_store_failure_calls == local_applies_before);
        assert(ble.apply_persistent_store_failure_calls ==
               global_applies_before);

        // Model a healthy verification result that began before the callback
        // and commits before the serialized failure event is consumed.
        ble.refresh_security(connection);
        assert(ble.security.security_ready_for_hid(generation, connection));
        assert(!controller.ble_link_ready());

        assert(controller.process_one_for_test());
        assert(!controller.ble_link_ready());
        assert(!ble.security_ready_for_hid(generation, connection));
        if (kind == hid_control_executor::BleEventKind::kStorageFailure) {
            assert(ble.apply_store_failure_calls == local_applies_before);
            assert(ble.apply_persistent_store_failure_calls ==
                   global_applies_before + 1);
            assert(controller.ble_snapshot().recovery_required);
            assert(!controller.ble_hid_peer_snapshot().active);
        } else {
            assert(ble.apply_store_failure_calls == local_applies_before + 1);
            assert(ble.apply_persistent_store_failure_calls ==
                   global_applies_before);
            assert(!controller.ble_snapshot().recovery_required);
            assert(ble.disconnect_calls == 1);
        }
    }
}

void test_store_failure_identity_reuse_is_fenced() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t reused_connection = 75;
    connect_ble(runtime, usb, ble, database, controller, reused_connection);
    const auto generation_a = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation_a, reused_connection);
    make_security_ready(ble);
    ble.refresh_security(reused_connection);
    assert(controller.ble_link_ready());

    // Capture immediate A/X inhibition without consuming its detailed event.
    assert(ble.security_inhibit.inhibit(generation_a, reused_connection, false));
    assert(!controller.ble_link_ready());
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     reused_connection));
    assert(controller.process_one_for_test());

    const auto generation_b = controller.ble_snapshot().generation;
    assert(generation_b != generation_a);
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     reused_connection));
    assert(controller.process_one_for_test());
    subscribe_composite(controller, database, generation_b, reused_connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(ble);
    ble.refresh_security(reused_connection);
    assert(controller.ble_link_ready());

    // Old A/X failure and verification traffic cannot affect reused B/X.
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kStoreFull, generation_a,
        reused_connection, -18));
    ble.security.apply_verification(generation_a, reused_connection,
                                    ble.security_link,
                                    ble.security_persisted);
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());
    assert(ble.apply_store_failure_calls == 0);
}

void test_disable_request_defers_security_retirement_to_executor() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 76;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    const auto security_before = ble.security.snapshot();
    const int retirements_before = ble.retire_security_calls;
    const auto disable = controller.request_ble_disable();
    assert(disable.action_result == ble_lifecycle::TransitionResult::kAccepted);

    // Lifecycle Stage A revokes public readiness immediately, but the UART-side
    // request path must not mutate executor-owned compound security state.
    assert(!controller.ble_link_ready());
    assert(controller.ble_hid_peer_snapshot().active);
    const auto security_after_request = ble.security.snapshot();
    assert(security_after_request.coherent && security_after_request.connected);
    assert(security_after_request.generation == security_before.generation);
    assert(security_after_request.connection_handle ==
           security_before.connection_handle);
    assert(ble.security.security_ready_for_hid(generation, connection));
    assert(ble.retire_security_calls == retirements_before);

    // Deterministically model verification winning the queue ordering. It can
    // update the old security snapshot, but cannot restore public authority.
    ble.refresh_security(connection);
    assert(ble.security.security_ready_for_hid(generation, connection));
    assert(!controller.ble_link_ready());
    assert(ble.retire_security_calls == retirements_before);

    assert(controller.process_one_for_test());
    assert(ble.retire_security_calls == retirements_before + 1);
    assert(!ble.security.snapshot().connected);
    assert(!controller.ble_hid_peer_snapshot().active);

    // A late verification for the retired identity is fenced by connected
    // state and cannot recreate readiness.
    ble.security.apply_verification(generation, connection, ble.security_link,
                                    ble.security_persisted);
    assert(!ble.security.snapshot().connected);
    assert(!ble.security.security_ready_for_hid(generation, connection));
    assert(!controller.ble_link_ready());
}

void test_fatal_storage_latch_preempts_disable_retirement() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 77;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    // Model callback capture separately from delayed executor event delivery.
    assert(ble.security_inhibit.inhibit(generation, connection, true));
    assert(ble.persistent_store_failure_observed());
    assert(!controller.ble_link_ready());
    const int advertising_calls = ble.advertising_calls;
    const int global_applies = ble.apply_persistent_store_failure_calls;

    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kStorageFailure, generation,
        connection, -31));

    // The first executor boundary reconciles the sticky fatal truth before a
    // queued disable can finish as healthy hidden/idle.
    assert(controller.process_one_for_test());
    assert(!ble.security.snapshot().connected);
    assert(!controller.ble_link_ready());
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(controller.ble_snapshot().recovery_required);
    assert(ble.apply_persistent_store_failure_calls == global_applies + 1);

    // The delayed detailed event is harmless and does not commit twice.
    assert(controller.process_one_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising && !failed.connected);
    assert(ble.apply_persistent_store_failure_calls == global_applies + 1);
    assert(!ble.security.snapshot().store_healthy);
    assert(ble.persistent_store_failure_observed());
    assert(ble.advertising_calls == advertising_calls);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
}

void test_fatal_storage_latch_preempts_disconnect_retirement() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 78;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    assert(ble.security_inhibit.inhibit(generation, connection, true));
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     connection));
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kStorageFailure, generation,
        connection, -32));
    const int advertising_calls = ble.advertising_calls;

    // The first executor boundary reconciles fatal truth before disconnect
    // processing can begin any normal readvertise path.
    assert(controller.process_one_for_test());
    assert(!ble.security.snapshot().connected);
    assert(!controller.ble_link_ready());
    assert(ble.advertising_calls == advertising_calls);
    assert(controller.ble_snapshot().recovery_required);
    assert(ble.apply_persistent_store_failure_calls == 1);

    // The queued detailed event is idempotent.
    assert(controller.process_one_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.desired == ble_lifecycle::DesiredExposure::kExposed);
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising && !failed.connected);
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(ble.persistent_store_failure_observed());
    assert(ble.advertising_calls == advertising_calls);
}

void fill_queue_with_stale_security_events(
    FakeBleBackend &ble, ble_lifecycle::Generation generation,
    std::uint16_t connection, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kEncryptionChange,
            generation - 1U, connection, 0));
    }
}

void publish_current_timeout_overflow_during_consume(
    hid_control_executor::Controller &controller) {
    assert(!controller.signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kTimeout,
        .generation = controller.ble_snapshot().generation,
        .connection_handle = ble_lifecycle::kNoConnection,
        .status = -77,
    }));
}

void drain_executor_before_fallback_publication(
    hid_control_executor::Controller &controller) {
    // This hook runs inside the failed producer after queue-full was observed,
    // but before the sticky fallback is published.  It deterministically
    // reproduces the old final-drain lost-wakeup window without sleeps.
    assert(controller.process_wake_cycle_for_test());
    assert(!controller.process_one_for_test());
}

void publish_lifecycle_fallback_while_executor_active(
    hid_control_executor::Controller &controller) {
    controller.signal_ble_lifecycle_handoff_failure();
}

void clear_retained_executor_wake(
    hid_control_executor::Controller &controller) {
    assert(controller.process_wake_cycle_for_test());
    assert(!controller.executor_wake_pending_for_test());
}

void test_reset_sync_normal_handoff_reaches_advertising() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    const auto next_generation = generation + 1U;

    ble.set_generation(next_generation);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -60));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().generation == next_generation);
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kEnabling);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kSync, next_generation));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kAdvertising);
    assert(!controller.ble_snapshot().recovery_required);
}

void test_sync_fallback_wakes_after_executor_final_queue_drain() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    const auto next_generation = generation + 1U;

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    ble.set_generation(next_generation);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -71));
    controller.set_ble_enqueue_failure_hook_for_test(
        hid_control_executor::Controller::BleEnqueueFailurePhase::
            kAfterGenericFallback,
        drain_executor_before_fallback_publication);

    assert(!ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kSync, next_generation));
    const auto stranded_without_wake = controller.ble_snapshot();
    assert(stranded_without_wake.observed ==
           ble_lifecycle::ObservedState::kEnabling);
    assert(!stranded_without_wake.advertising);
    assert(controller.executor_wake_pending_for_test());

    assert(controller.process_wake_cycle_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(ble.advertising_calls == 1);
}

void test_generic_overflow_wakes_after_executor_final_queue_drain() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t orphan = 106;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    ble.orphan_terminate_result = -82;

    fill_queue_with_stale_security_events(
        ble, generation, orphan,
        hid_control_executor::Controller::kActionQueueDepth);
    controller.set_ble_enqueue_failure_hook_for_test(
        hid_control_executor::Controller::BleEnqueueFailurePhase::
            kBeforeGenericFallback,
        drain_executor_before_fallback_publication);
    assert(!ble.event(hid_control_executor::BleEventKind::kConnect, orphan));
    assert(ble.orphan_terminate_calls == 1);
    assert(ble.last_orphan_connection == orphan);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());
    assert(controller.executor_wake_pending_for_test());

    assert(controller.process_wake_cycle_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kQueueOverflow);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(ble.advertising_calls == 1);
}

void test_adopted_peer_overflow_wakes_after_final_queue_drain() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 107;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    controller.set_ble_enqueue_failure_hook_for_test(
        hid_control_executor::Controller::BleEnqueueFailurePhase::
            kBeforeGenericFallback,
        drain_executor_before_fallback_publication);
    assert(!queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(!controller.ble_link_ready());
    assert(controller.executor_wake_pending_for_test());

    assert(controller.process_wake_cycle_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!ble.security_ready_for_hid(generation, connection));
    assert(ble.disconnect_calls == 1);
}

void test_fallback_wake_retention_active_and_coalesced_priority() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        advertise_ble(runtime, usb, ble, database, controller);
        clear_retained_executor_wake(controller);

        // Publication before a wait (equivalently while the task is blocked)
        // remains pending until the executor takes the wake.
        controller.signal_ble_lifecycle_handoff_failure();
        assert(controller.executor_wake_pending_for_test());
        assert(controller.process_wake_cycle_for_test());
        assert(controller.ble_snapshot().recovery_required);
        assert(controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kFault);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        advertise_ble(runtime, usb, ble, database, controller);
        clear_retained_executor_wake(controller);
        const auto generation = controller.ble_snapshot().generation;

        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kEncryptionChange,
            generation - 1U, ble_lifecycle::kNoConnection));
        controller.set_process_after_reconciliation_hook_for_test(
            publish_lifecycle_fallback_while_executor_active);
        assert(controller.process_wake_cycle_for_test());
        // The final reconciliation catches publication during active work;
        // the retained notification can cause a harmless follow-up pass.
        assert(controller.ble_snapshot().recovery_required);
        assert(controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kFault);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        advertise_ble(runtime, usb, ble, database, controller);
        clear_retained_executor_wake(controller);
        const auto generation = controller.ble_snapshot().generation;

        fill_queue_with_stale_security_events(
            ble, generation, ble_lifecycle::kNoConnection,
            hid_control_executor::Controller::kActionQueueDepth);
        assert(!ble.event(hid_control_executor::BleEventKind::kTimeout,
                          ble_lifecycle::kNoConnection, -83));
        controller.signal_ble_lifecycle_handoff_failure();
        // The native boolean models coalesced task notifications: both sticky
        // causes require only one wake cycle, with lifecycle outranking generic.
        assert(controller.executor_wake_pending_for_test());
        assert(controller.process_wake_cycle_for_test());
        assert(!controller.executor_wake_pending_for_test());
        assert(controller.ble_snapshot().recovery_required);
        assert(controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kFault);
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kQueueOverflow);
    }
}

void test_dropped_future_sync_cannot_leave_reset_enabling() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    const auto next_generation = generation + 1U;

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    ble.set_generation(next_generation);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -61));
    assert(!ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kSync, next_generation));
    assert(!controller.ble_link_ready());

    assert(controller.process_one_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.generation == generation);
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kQueueOverflow);
    while (controller.process_one_for_test()) {
    }
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
}

void test_dropped_future_lifecycle_timeout_cannot_leave_reset_enabling() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    const auto next_generation = generation + 1U;

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    ble.set_generation(next_generation);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -66));
    assert(!ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kTimeout, next_generation, -3));

    assert(controller.process_one_for_test());
    while (controller.process_one_for_test()) {
    }
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kQueueOverflow);
}

void test_stale_sync_does_not_set_handoff_failure() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event_for_generation(
        hid_control_executor::BleEventKind::kSync, generation - 1U));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kAdvertising);
    assert(!controller.ble_snapshot().recovery_required);
}

void test_repeated_reset_publication_failure_is_bounded() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    ble.set_generation(generation + 1U);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -62));
    ble.set_generation(generation + 2U);
    assert(!ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation + 1U, -63));
    assert(controller.process_one_for_test());
    while (controller.process_one_for_test()) {
    }
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
}

void test_reset_sync_handoff_wrap_failure_remains_visible() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    controller.set_ble_generation_for_test(
        std::numeric_limits<ble_lifecycle::Generation>::max() - 1U);
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    assert(generation ==
           std::numeric_limits<ble_lifecycle::Generation>::max());

    fill_queue_with_stale_security_events(
        ble, generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    ble.set_generation(0);
    assert(ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kReset, generation, -64));
    assert(!ble.lifecycle_event_for_generation(
        hid_control_executor::BleEventKind::kSync, 0));
    assert(controller.process_one_for_test());
    while (controller.process_one_for_test()) {
    }
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
}

void test_dropped_connect_terminates_exact_orphan_and_ignores_disconnect() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t orphan = 97;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    const int advertising_calls = ble.advertising_calls;

    fill_queue_with_stale_security_events(
        ble, generation, orphan,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kConnect, orphan));
    assert(ble.orphan_terminate_calls == 1);
    assert(ble.last_orphan_connection == orphan);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());

    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kQueueOverflow);
    assert(ble.disconnect_calls == 0);
    while (controller.process_one_for_test()) {
    }

    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kDisconnect, generation, orphan));
    assert(controller.process_one_for_test());
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kQueueOverflow);
    assert(ble.advertising_calls == advertising_calls);
    assert(ble.orphan_terminate_calls == 1);
}

void test_dropped_connect_termination_failure_stays_fail_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t orphan = 98;
    advertise_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    ble.orphan_terminate_result = -65;

    fill_queue_with_stale_security_events(
        ble, generation, orphan,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kConnect, orphan));
    assert(ble.orphan_terminate_calls == 1);
    assert(ble.last_orphan_connection == orphan);
    assert(controller.process_one_for_test());
    while (controller.process_one_for_test()) {
    }
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(ble.orphan_terminate_calls == 1);
}

void test_overflow_current_then_stale_preserves_cccd_fail_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 90;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(!controller.ble_link_ready());
    assert(!ble.event_for_generation(
        hid_control_executor::BleEventKind::kEncryptionChange,
        generation - 1U, connection));

    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());
    assert(ble.disconnect_calls == 1);
}

void test_overflow_stale_then_current_security_fails_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 91;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event_for_generation(
        hid_control_executor::BleEventKind::kEncryptionChange,
        generation - 1U, connection));
    assert(controller.ble_link_ready());
    assert(!ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                      connection));
    assert(!controller.ble_link_ready());

    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!ble.security_ready_for_hid(generation, connection));
    assert(ble.disconnect_calls == 1);
}

void test_two_current_overflows_preserve_suspend_fail_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 92;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!queue_control_point(controller, generation, connection,
                                database.handles.control_point_value, true));
    assert(!controller.ble_link_ready());
    assert(!ble.event(hid_control_executor::BleEventKind::kPairingTimeout,
                      connection));

    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(ble.disconnect_calls == 1);
    while (controller.process_one_for_test()) {
    }
    assert(ble.disconnect_calls == 1);
}

void test_stale_overflow_does_not_poison_reused_handle_authority() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 93;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation_a = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation_a, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation_a, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                      connection));
    hid_control_executor::Controller::Action deferred{};
    assert(controller.dequeue_one_for_test(deferred));
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    controller.process_for_test(deferred);
    assert(!controller.ble_snapshot().recovery_required);
    while (controller.process_one_for_test()) {
    }

    const auto generation_b = controller.ble_snapshot().generation;
    assert(generation_b != generation_a);
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     connection));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kIdle);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     connection));
    assert(controller.process_one_for_test());
    const auto generation_c = controller.ble_snapshot().generation;
    assert(generation_c != generation_a && generation_c != generation_b);
    subscribe_composite(controller, database, generation_c, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    for (std::size_t index = 0;
         index < hid_control_executor::Controller::kActionQueueDepth; ++index) {
        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kEncryptionChange,
            generation_a, connection));
    }
    assert(!ble.event_for_generation(
        hid_control_executor::BleEventKind::kEncryptionChange,
        generation_a, connection));
    assert(controller.ble_link_ready());
    assert(controller.process_one_for_test());
    assert(controller.ble_link_ready());
    assert(!controller.ble_snapshot().recovery_required);
}

void test_overflow_consumer_cas_preserves_racing_current_producer() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 94;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation_a = controller.ble_snapshot().generation;

    fill_queue_with_stale_security_events(
        ble, generation_a, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                      connection));
    hid_control_executor::Controller::Action deferred{};
    assert(controller.dequeue_one_for_test(deferred));
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.ble_snapshot().generation != generation_a);
    controller.set_overflow_consume_hook_for_test(
        publish_current_timeout_overflow_during_consume);
    controller.process_for_test(deferred);

    assert(controller.ble_snapshot().recovery_required);
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(ble.disconnect_calls == 1);
}

void test_dropped_store_full_uses_generic_fault_not_global_fatal() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 95;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kStoreFull,
                      connection, -45));
    assert(!ble.persistent_store_failure_observed());
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(ble.apply_persistent_store_failure_calls == 0);
    assert(!ble.persistent_store_failure_observed());
}

void test_overflow_generation_zero_wrap_remains_fail_closed() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 96;
    controller.set_ble_generation_for_test(
        std::numeric_limits<ble_lifecycle::Generation>::max());
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    assert(generation == 0);
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(!controller.ble_link_ready());
    assert(!ble.event_for_generation(
        hid_control_executor::BleEventKind::kEncryptionChange,
        std::numeric_limits<ble_lifecycle::Generation>::max(), connection));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().recovery_required);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(ble.disconnect_calls == 1);
}

void test_fatal_storage_overflow_survives_disable_generation_advance() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 87;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());

    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kStorageFailure,
                      connection, -41));
    assert(ble.persistent_store_failure_observed());
    assert(!controller.ble_link_ready());

    // Model the SMP execution window after xQueueReceive frees one slot but
    // before the executor processes that action. UART disable Stage A can run
    // concurrently on the other core and advance the lifecycle generation.
    hid_control_executor::Controller::Action deferred{};
    assert(controller.dequeue_one_for_test(deferred));
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.ble_snapshot().generation == generation + 1U);
    controller.process_for_test(deferred);

    const auto failed = controller.ble_snapshot();
    assert(failed.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising && !failed.connected);
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStorage);
    assert(!ble.security.snapshot().store_healthy);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());

    while (controller.process_one_for_test()) {
    }
    const auto terminal = controller.ble_snapshot();
    assert(terminal.observed == ble_lifecycle::ObservedState::kFault);
    assert(terminal.recovery_required);
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
}

void test_fatal_storage_overflow_survives_disconnect() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 88;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     connection));
    fill_queue_with_stale_security_events(
        ble, generation, connection,
        hid_control_executor::Controller::kActionQueueDepth - 1U);
    assert(!ble.event(hid_control_executor::BleEventKind::kStorageFailure,
                      connection, -42));

    assert(controller.process_one_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising && !failed.connected);
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(ble.persistent_store_failure_observed());
    assert(!ble.security.snapshot().store_healthy);
    assert(!controller.ble_link_ready());
}

void test_fatal_storage_without_connection_preempts_enable_and_is_idempotent() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    assert(ble.event(hid_control_executor::BleEventKind::kSync));
    assert(controller.process_one_for_test());
    assert(!controller.ble_snapshot().connected);
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto hidden = controller.ble_snapshot();
    assert(hidden.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(hidden.observed == ble_lifecycle::ObservedState::kIdle);

    fill_queue_with_stale_security_events(
        ble, hidden.generation, ble_lifecycle::kNoConnection,
        hid_control_executor::Controller::kActionQueueDepth);
    assert(!ble.event(hid_control_executor::BleEventKind::kStorageFailure,
                      ble_lifecycle::kNoConnection, -43));
    hid_control_executor::Controller::Action deferred{};
    assert(controller.dequeue_one_for_test(deferred));
    const int advertising_calls = ble.advertising_calls;
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    controller.process_for_test(deferred);
    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.advertising && !failed.connected);
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(controller.active_operation_for_test() == ControlOperation::kNone);

    while (controller.process_one_for_test()) {
    }
    assert(ble.advertising_calls == advertising_calls);

    assert(ble.event(hid_control_executor::BleEventKind::kStorageFailure,
                     ble_lifecycle::kNoConnection, -44));
    assert(controller.process_one_for_test());
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(controller.ble_snapshot().observed ==
           ble_lifecycle::ObservedState::kFault);
    assert(controller.ble_snapshot().recovery_required);
}

void test_startup_store_recovery_failure_never_exposes_ble() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    ble.initialize_result = -147;
    ble.initialize_persistent_failure = true;

    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.stack_ready);
    assert(!failed.advertising && !failed.connected);
    assert(ble.initialize_calls == 1);
    assert(ble.advertising_attempts == 0 && ble.advertising_calls == 0);
    assert(ble.initiate_security_calls == 0);

    // The immediate backend latch makes administration truthful even before
    // the detailed queued event is consumed.
    assert(controller.request_bond_list().kind ==
           hid_control_executor::BleBondListResultKind::kStorageFailure);
    assert(controller.request_bond_remove(executor_bond_id('7')).kind ==
           hid_control_executor::BleBondRemoveResultKind::kStorageFailure);
    assert(ble.bond_list_calls == 0 && ble.bond_remove_calls == 0);

    while (controller.process_one_for_test()) {
    }
    assert(ble.apply_persistent_store_failure_calls == 1);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStorage);
    assert(ble.advertising_attempts == 0);
}

void test_store_full_does_not_trigger_global_fatal_reconciliation() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 89;
    connect_ble(runtime, usb, ble, database, controller, connection);
    assert(ble.event(hid_control_executor::BleEventKind::kStoreFull,
                     connection, -45));
    assert(controller.process_one_for_test());
    assert(!ble.persistent_store_failure_observed());
    assert(ble.apply_persistent_store_failure_calls == 0);
    assert(!controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStoreFull);
}

void test_store_full_retirement_does_not_poison_future_peer() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t reused_connection = 79;
    connect_ble(runtime, usb, ble, database, controller, reused_connection);
    const auto generation_a = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation_a, reused_connection);
    make_security_ready(ble);
    ble.refresh_security(reused_connection);
    assert(controller.ble_link_ready());

    assert(ble.security_inhibit.inhibit(generation_a, reused_connection, false));
    assert(!controller.ble_link_ready());
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kStoreFull, generation_a,
        reused_connection, -33));

    assert(controller.process_one_for_test());
    assert(controller.process_one_for_test());
    assert(!controller.ble_snapshot().recovery_required);
    assert(!ble.persistent_store_failure_observed());
    assert(ble.apply_store_failure_calls == 0);
    assert(ble.apply_persistent_store_failure_calls == 0);

    // Finish expected disable, then reuse the numeric handle under a fresh
    // generation. The old local inhibit and stale StoreFull remain fenced.
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect,
                     reused_connection));
    assert(controller.process_one_for_test());
    assert(controller.request_ble_enable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto generation_b = controller.ble_snapshot().generation;
    assert(generation_b != generation_a);
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     reused_connection));
    assert(controller.process_one_for_test());
    subscribe_composite(controller, database, generation_b, reused_connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(ble);
    ble.refresh_security(reused_connection);
    assert(controller.ble_link_ready());
    assert(!controller.ble_snapshot().recovery_required);
}

void test_internal_ble_notification_adapter_and_result_model() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 73;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    const hid_control_executor::BleHidWorkIdentity keyboard_identity{
        .generation = generation,
        .connection_handle = connection,
        .characteristic_handle = database.handles.keyboard_value,
    };
    const hid_control_executor::BleKeyboardReport keyboard{
        0x02, 0x00, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(controller.submit_ble_keyboard(keyboard_identity, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kNotReady);
    assert(database.notify_calls == 0);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());
    assert(controller.submit_ble_keyboard(keyboard_identity, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kStackAccepted);
    assert(database.notify_calls == 1);
    assert(database.last_connection == connection);
    assert(database.last_characteristic == database.handles.keyboard_value);
    assert(database.last_payload_length == 8);
    assert(database.last_payload == keyboard);

    const hid_control_executor::BleHidWorkIdentity mouse_identity{
        .generation = generation,
        .connection_handle = connection,
        .characteristic_handle = database.handles.mouse_value,
    };
    const hid_control_executor::BleMouseReport mouse{
        0xff, 0x81, 0x7f, 0xfe, 0x01};
    assert(controller.submit_ble_mouse(mouse_identity, mouse) ==
           hid_control_executor::BleHidSubmitResult::kStackAccepted);
    assert(database.notify_calls == 2);
    assert(database.last_characteristic == database.handles.mouse_value);
    assert(database.last_payload_length == 5);
    assert(database.last_payload[0] == 0x1f);
    for (std::size_t index = 1; index < mouse.size(); ++index) {
        assert(database.last_payload[index] == mouse[index]);
    }
    static_assert(hid_control_executor::kBleKeyboardAllUp.size() == 8);
    static_assert(hid_control_executor::kBleMouseAllUp.size() == 5);
    for (const auto byte : hid_control_executor::kBleKeyboardAllUp) assert(byte == 0);
    for (const auto byte : hid_control_executor::kBleMouseAllUp) assert(byte == 0);

    auto stale = keyboard_identity;
    ++stale.generation;
    assert(controller.submit_ble_keyboard(stale, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kStale);
    stale = keyboard_identity;
    ++stale.characteristic_handle;
    assert(controller.submit_ble_keyboard(stale, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kStale);
    assert(database.notify_calls == 2);

    database.notify_result =
        hid_control_executor::BleNotifyBackendResult::kResourceFailure;
    assert(controller.submit_ble_keyboard(keyboard_identity, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kResourceFailure);
    assert(database.notify_calls == 3);
    database.notify_result =
        hid_control_executor::BleNotifyBackendResult::kStackRejected;
    assert(controller.submit_ble_keyboard(keyboard_identity, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kStackRejected);
    assert(database.notify_calls == 4);  // exactly one call per request; no retry

    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        database.handles.keyboard_value, false));
    assert(controller.process_one_for_test());
    assert(controller.submit_ble_keyboard(keyboard_identity, keyboard) ==
           hid_control_executor::BleHidSubmitResult::kNotReady);
    assert(database.notify_calls == 4);
    assert(queue_subscription(
        controller, generation, connection,
        hid_control_executor::BleHidInterface::kMouse,
        database.handles.mouse_value, false));
    assert(controller.process_one_for_test());
    assert(controller.submit_ble_mouse(mouse_identity, mouse) ==
           hid_control_executor::BleHidSubmitResult::kNotReady);
    assert(database.notify_calls == 4);
}

struct ReadyBleRouteFixture {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    std::uint16_t connection = 81;
    ble_lifecycle::Generation generation = 0;

    explicit ReadyBleRouteFixture(std::uint16_t handle = 81)
        : connection(handle) {
        connect_ble(runtime, usb, ble, database, controller, connection);
        generation = controller.ble_snapshot().generation;
        subscribe_composite(controller, database, generation, connection);
        make_security_ready(ble);
        ble.refresh_security(connection);
        assert(controller.ble_link_ready());
        const auto activation =
            controller.request_route(hid_route::OutputRoute::kBle);
        assert(activation.action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
        assert(runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
    }
};

void observe_exact_route_disconnect(ReadyBleRouteFixture &fixture);

void test_internal_ble_route_activation_and_exact_payloads() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 80);
        assert(controller.activate_ble_route_internal().action_result ==
               hid_runtime::RouteTransitionResult::kNotReady);
        assert(runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
        assert(controller.request_route(hid_route::OutputRoute::kBle)
                   .action_result ==
               hid_runtime::RouteTransitionResult::kNotReady);
    }

    ReadyBleRouteFixture fixture;
    const auto authority =
        fixture.runtime.state_machine().ble_route_authority_snapshot();
    assert(authority.active && authority.coherent);
    assert(authority.authority_epoch ==
           fixture.runtime.state_machine().authority_epoch());
    assert(authority.route_generation ==
           fixture.runtime.state_machine().route_snapshot().generation);
    assert(authority.ble_generation == fixture.generation);
    assert(authority.connection_handle == fixture.connection);
    assert(authority.keyboard_characteristic_handle ==
           fixture.database.handles.keyboard_value);
    assert(authority.mouse_characteristic_handle ==
           fixture.database.handles.mouse_value);
    const auto status = fixture.controller.route_snapshot();
    assert(status.route.desired == hid_route::OutputRoute::kBle);
    assert(status.route.active == hid_route::OutputRoute::kBle);
    assert(status.route.transition == hid_route::Transition::kStable);
    assert(status.ready);
    const auto noop =
        fixture.controller.request_route(hid_route::OutputRoute::kBle);
    assert(noop.action_result == hid_runtime::RouteTransitionResult::kNoOp);
    assert(noop.snapshot_valid && noop.snapshot.ready);
    assert(noop.snapshot.route.generation == status.route.generation);

    const std::array<std::uint8_t, 6> keys{0x73, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0x02, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 1);
    assert(fixture.database.last_connection == fixture.connection);
    assert(fixture.database.last_characteristic ==
           fixture.database.handles.keyboard_value);
    assert(fixture.database.last_payload_length == 8);
    const std::array<std::uint8_t, 8> expected_keyboard{
        0x02, 0, 0x73, 0, 0, 0, 0, 0};
    assert(fixture.database.last_payload == expected_keyboard);
    assert(fixture.runtime.state_machine().keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kSubmitted);
    fixture.runtime.state_machine().finalize_keyboard_report();

    assert(fixture.controller.queue_ble_mouse_report(
               0xff, -127, 127, -2, 1) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 2);
    assert(fixture.database.last_characteristic ==
           fixture.database.handles.mouse_value);
    assert(fixture.database.last_payload_length == 5);
    assert(fixture.database.last_payload[0] == 0x1f);
    assert(fixture.database.last_payload[1] == 0x81);
    assert(fixture.database.last_payload[2] == 0x7f);
    assert(fixture.database.last_payload[3] == 0xfe);
    assert(fixture.database.last_payload[4] == 0x01);
    fixture.runtime.state_machine().finalize_mouse_report();
}

void test_ble_route_none_usb_and_no_dual_delivery() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &usb, &ble, &database));
        const std::array<std::uint8_t, 6> keys{0x73, 0, 0, 0, 0, 0};
        assert(controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kNotReady);
        assert(database.notify_calls == 0);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        complete_attach(runtime, usb, controller);
        mount_ready(runtime);
        assert(runtime.state_machine().request_route_usb().action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
        ReportSink sink;
        const std::array<std::uint8_t, 6> keys{0x73, 0, 0, 0, 0, 0};
        assert(runtime.state_machine().queue_keyboard_report(0, keys));
        runtime.state_machine().execute(ReportSink::submit, &sink);
        assert(sink.calls == 1);
        assert(database.notify_calls == 0);
    }
    ReadyBleRouteFixture fixture(82);
    ReportSink usb_sink;
    const std::array<std::uint8_t, 6> keys{0x72, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    fixture.runtime.state_machine().execute(ReportSink::submit, &usb_sink);
    assert(usb_sink.calls == 0);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 1);
    assert(usb_sink.calls == 0);
}

void test_ble_work_token_fences_every_authority_field() {
    ReadyBleRouteFixture fixture(83);
    const std::array<std::uint8_t, 6> keys{0x71, 0, 0, 0, 0, 0};
    for (int mutation = 0; mutation < 6; ++mutation) {
        assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        hid_control_executor::Controller::Action work{};
        assert(fixture.controller.dequeue_one_for_test(work));
        const auto original = work.hid_work;
        switch (mutation) {
            case 0: ++work.hid_work.route_generation; break;
            case 1: ++work.hid_work.authority_epoch; break;
            case 2: ++work.hid_work.transport_generation; break;
            case 3: ++work.hid_work.connection_handle; break;
            case 4: ++work.hid_work.characteristic_handle; break;
            case 5:
                work.hid_work.report_kind =
                    hid_runtime::ReportKind::kUnsafeMouse;
                break;
        }
        fixture.controller.process_for_test(work);
        assert(fixture.database.notify_calls == 0);
        assert(fixture.runtime.state_machine().keyboard_report_snapshot().state ==
               hid_runtime::KeyboardReportTicketState::kPublished);
        assert(fixture.runtime.state_machine().cancel_keyboard_report());
        fixture.runtime.state_machine().abandon_ble_report(
            hid_runtime::Interface::kKeyboard, original);
        fixture.runtime.state_machine().finalize_keyboard_report();
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
    }

    assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    hid_control_executor::Controller::Action stale{};
    assert(fixture.controller.dequeue_one_for_test(stale));
    const auto authority =
        fixture.runtime.state_machine().ble_route_authority_snapshot();
    assert(fixture.runtime.state_machine().retire_ble_route_if_matches(
        authority));
    fixture.controller.process_for_test(stale);
    assert(fixture.database.notify_calls == 2);
    assert(fixture.runtime.state_machine().route_snapshot().transition ==
           hid_route::Transition::kReleasing);
}

void test_canceled_ble_ticket_waits_for_exact_action_acknowledgment() {
    ReadyBleRouteFixture fixture(96);
    const std::array<std::uint8_t, 6> old_keys{0x6c, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, old_keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    hid_control_executor::Controller::Action old_action{};
    assert(fixture.controller.dequeue_one_for_test(old_action));
    assert(fixture.runtime.state_machine().cancel_keyboard_report());
    fixture.runtime.state_machine().finalize_keyboard_report();
    assert(fixture.runtime.state_machine().keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kCanceled);

    const std::array<std::uint8_t, 6> new_keys{0x6b, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, new_keys) ==
           hid_runtime::KeyboardReportBeginResult::kBusy);
    fixture.controller.process_for_test(old_action);
    assert(fixture.database.notify_calls == 0);
    assert(fixture.runtime.state_machine().keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kCanceled);

    assert(fixture.controller.queue_ble_keyboard_report(0, new_keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 1);
    assert(fixture.database.last_payload[2] == 0x6b);
}

void test_usb_mount_retires_ble_authority_without_forgetting_held_state() {
    ReadyBleRouteFixture fixture(97);
    const std::array<std::uint8_t, 6> keys{0x6a, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(fixture.controller.process_one_for_test());
    fixture.runtime.state_machine().finalize_keyboard_report();
    assert(fixture.runtime.state_machine().keyboard_state().keycodes[0] ==
           0x6a);
    assert(fixture.controller.process_wake_cycle_for_test());

    assert(action(fixture.controller.request_attach()) ==
           usb_lifecycle::TransitionResult::kAccepted);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.controller.process_wake_cycle_for_test());
    fixture.runtime.state_machine().on_mount();

    // Native tests call StateMachine directly, so explicitly model Runtime's
    // production callback-to-executor authority wake.
    fixture.controller.signal_hid_authority_change();
    assert(fixture.controller.process_wake_cycle_for_test());
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kBle);
    assert(fixture.runtime.state_machine().route_snapshot().transition ==
           hid_route::Transition::kReleasing);
    assert(fixture.runtime.state_machine().safety_required(
        hid_runtime::Interface::kKeyboard));
    assert(fixture.runtime.state_machine().keyboard_state().keycodes[0] ==
           0x6a);
    assert(fixture.database.notify_calls == 3);
}

void test_ble_readiness_loss_retires_route_without_auto_restore() {
    for (int loss = 0; loss < 3; ++loss) {
        ReadyBleRouteFixture fixture(static_cast<std::uint16_t>(84 + loss));
        const std::array<std::uint8_t, 6> keys{0x70, 0, 0, 0, 0, 0};
        assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        hid_control_executor::Controller::Action delayed{};
        assert(fixture.controller.dequeue_one_for_test(delayed));
        if (loss == 0) {
            assert(queue_subscription(
                fixture.controller, fixture.generation, fixture.connection,
                hid_control_executor::BleHidInterface::kKeyboard,
                fixture.database.handles.keyboard_value, false));
        } else if (loss == 1) {
            assert(queue_subscription(
                fixture.controller, fixture.generation, fixture.connection,
                hid_control_executor::BleHidInterface::kMouse,
                fixture.database.handles.mouse_value, false));
        } else {
            assert(queue_control_point(
                fixture.controller, fixture.generation, fixture.connection,
                fixture.database.handles.control_point_value, true));
        }
        assert(fixture.controller.process_one_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        fixture.controller.process_for_test(delayed);
        const int safety_calls = loss < 2 ? 1 : 0;
        assert(fixture.database.notify_calls == safety_calls);

        if (loss == 0) {
            assert(queue_subscription(
                fixture.controller, fixture.generation, fixture.connection,
                hid_control_executor::BleHidInterface::kKeyboard,
                fixture.database.handles.keyboard_value, true));
        } else if (loss == 1) {
            assert(queue_subscription(
                fixture.controller, fixture.generation, fixture.connection,
                hid_control_executor::BleHidInterface::kMouse,
                fixture.database.handles.mouse_value, true));
        } else {
            assert(queue_control_point(
                fixture.controller, fixture.generation, fixture.connection,
                fixture.database.handles.control_point_value, false));
        }
        assert(fixture.controller.process_one_for_test());
        assert(fixture.controller.ble_link_ready());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kNotReady);
        assert(fixture.database.notify_calls == safety_calls);
        assert(fixture.controller.expire_ble_route_release_grace_for_test());
        assert(fixture.controller.process_one_for_test());
        assert(fixture.ble.disconnect_calls == 1);
        assert(fixture.ble.last_connection == fixture.connection);
        assert(fixture.ble.event(
            hid_control_executor::BleEventKind::kDisconnect,
            fixture.connection));
        assert(fixture.controller.process_one_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
}

void test_callback_readiness_loss_preempts_earlier_queued_report() {
    ReadyBleRouteFixture fixture(91);
    const std::array<std::uint8_t, 6> keys{0x6e, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    // The report action is already first in the FIFO. Callback-side loss
    // publication must nevertheless close readiness before that action can
    // reach the notification backend.
    assert(queue_subscription(
        fixture.controller, fixture.generation, fixture.connection,
        hid_control_executor::BleHidInterface::kKeyboard,
        fixture.database.handles.keyboard_value, false));
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 0);
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kBle);
    assert(fixture.controller.process_one_for_test());
    assert(fixture.database.notify_calls == 1);
}

void test_security_and_storage_loss_preempt_ble_work() {
    {
        ReadyBleRouteFixture fixture(92);
        assert(fixture.controller.queue_ble_mouse_report(0, 1, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        fixture.ble.security_link.authenticated = false;
        assert(fixture.ble.event(
            hid_control_executor::BleEventKind::kEncryptionChange,
            fixture.connection, 0));
        assert(fixture.controller.process_one_for_test());
        assert(fixture.database.notify_calls == 0);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.controller.process_one_for_test());
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
    {
        ReadyBleRouteFixture fixture(93);
        const std::array<std::uint8_t, 6> keys{0x6d, 0, 0, 0, 0, 0};
        assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(fixture.ble.event(
            hid_control_executor::BleEventKind::kStorageFailure,
            fixture.connection, -44));
        assert(fixture.controller.process_one_for_test());
        assert(fixture.database.notify_calls == 0);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.controller.process_one_for_test());
        assert(fixture.controller.ble_snapshot().recovery_required);
        assert(fixture.controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kStorage);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
}

void test_overflow_and_store_full_retire_only_current_ble_route() {
    {
        ReadyBleRouteFixture fixture(94);
        for (int index = 0;
             index < static_cast<int>(
                         hid_control_executor::Controller::kActionQueueDepth);
             ++index) {
            assert(fixture.controller.signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kEncryptionChange,
                .generation = fixture.generation - 1,
                .connection_handle = fixture.connection,
            }));
        }
        assert(!queue_subscription(
            fixture.controller, fixture.generation, fixture.connection,
            hid_control_executor::BleHidInterface::kKeyboard,
            fixture.database.handles.keyboard_value, false));
        assert(fixture.controller.process_wake_cycle_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.controller.ble_snapshot().recovery_required);
        assert(fixture.database.notify_calls == 0);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
    {
        ReadyBleRouteFixture fixture(95);
        assert(fixture.ble.event(
            hid_control_executor::BleEventKind::kStoreFull,
            fixture.connection, -55));
        assert(fixture.controller.process_one_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(!fixture.controller.ble_snapshot().recovery_required);
        assert(fixture.database.notify_calls == 0);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
}

void test_ble_backend_failure_retires_and_never_replays_mouse() {
    for (const auto failure : {
             hid_control_executor::BleNotifyBackendResult::kResourceFailure,
             hid_control_executor::BleNotifyBackendResult::kStackRejected}) {
        ReadyBleRouteFixture fixture(
            failure == hid_control_executor::BleNotifyBackendResult::kResourceFailure
                ? 87
                : 88);
        fixture.database.notify_result = failure;
        assert(fixture.controller.queue_ble_mouse_report(1, 7, -3, 2, -1) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(fixture.controller.process_one_for_test());
        assert(fixture.database.notify_calls == 3);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.runtime.state_machine().host_state_uncertain(
            hid_runtime::Interface::kMouse));
        assert(fixture.runtime.state_machine().safety_required(
            hid_runtime::Interface::kMouse));
        assert(!fixture.controller.process_one_for_test());
        assert(fixture.database.notify_calls == 3);
        assert(fixture.controller.queue_ble_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kNotReady);
        assert(fixture.database.notify_calls == 3);
    }
}

void test_ble_not_ready_and_stale_results_terminalize_runtime_ticket() {
    for (const auto result : {hid_runtime::BleSubmitResult::kNotReady,
                              hid_runtime::BleSubmitResult::kStale}) {
        ReadyBleRouteFixture fixture(
            result == hid_runtime::BleSubmitResult::kNotReady ? 98 : 99);
        const std::array<std::uint8_t, 6> keys{0x69, 0, 0, 0, 0, 0};
        assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        hid_control_executor::Controller::Action action{};
        assert(fixture.controller.dequeue_one_for_test(action));
        BleSubmitSink sink{.result = result};
        assert(!fixture.runtime.state_machine().process_ble_report(
            action.hid_interface, action.hid_work, BleSubmitSink::submit,
            &sink));
        assert(sink.calls == 1);
        assert(fixture.database.notify_calls == 0);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        assert(fixture.runtime.state_machine().keyboard_report_snapshot().state ==
               hid_runtime::KeyboardReportTicketState::kCanceled);
        fixture.runtime.state_machine().finalize_keyboard_report();
        assert(!fixture.controller.process_one_for_test());
        assert(sink.calls == 1);
        assert(fixture.database.notify_calls == 0);
    }
}

void test_ble_disconnect_reconnect_kills_old_work_and_route() {
    ReadyBleRouteFixture fixture(89);
    const std::array<std::uint8_t, 6> keys{0x6f, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    hid_control_executor::Controller::Action old_work{};
    assert(fixture.controller.dequeue_one_for_test(old_work));
    assert(fixture.ble.event(hid_control_executor::BleEventKind::kDisconnect,
                             fixture.connection));
    const auto status = fixture.controller.route_snapshot();
    assert(status.route.desired == hid_route::OutputRoute::kNone);
    assert(status.route.active == hid_route::OutputRoute::kNone);
    assert(status.route.transition == hid_route::Transition::kStable);
    assert(!status.ready);
    fixture.controller.process_for_test(old_work);
    assert(fixture.database.notify_calls == 0);

    constexpr std::uint16_t new_connection = 90;
    assert(fixture.ble.event(hid_control_executor::BleEventKind::kConnect,
                             new_connection));
    assert(fixture.controller.process_one_for_test());
    const auto new_generation = fixture.controller.ble_snapshot().generation;
    subscribe_composite(fixture.controller, fixture.database, new_generation,
                        new_connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(fixture.ble);
    fixture.ble.refresh_security(new_connection);
    assert(fixture.controller.ble_link_ready());
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(fixture.database.notify_calls == 0);
}

void begin_explicit_ble_route_retirement(ReadyBleRouteFixture &fixture) {
    const auto outcome =
        fixture.controller.request_route(hid_route::OutputRoute::kNone);
    assert(outcome.action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(outcome.snapshot.route.transition ==
           hid_route::Transition::kReleasing);
    assert(fixture.controller.process_wake_cycle_for_test());
}

void observe_exact_route_disconnect(ReadyBleRouteFixture &fixture) {
    assert(fixture.ble.event(hid_control_executor::BleEventKind::kDisconnect,
                             fixture.connection));
    assert(fixture.controller.process_one_for_test());
}

void test_u74c_normal_retirement_release_grace_and_cross_transport() {
    static_assert(hid_control_executor::kBleRouteReleaseGraceMs == 100);
    ReadyBleRouteFixture fixture(120);
    const auto initial_route = fixture.runtime.state_machine().route_snapshot();
    const auto initial_authority =
        fixture.runtime.state_machine().ble_route_authority_snapshot();
    const std::array<std::uint8_t, 6> queued_keys{0x73, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, queued_keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);

    // C1/C2/C30: Stage A revokes ordinary authority before the already queued
    // normal action can run. Only the two safety reports reach the backend.
    begin_explicit_ble_route_retirement(fixture);
    const auto releasing = fixture.runtime.state_machine().route_snapshot();
    assert(releasing.desired == hid_route::OutputRoute::kNone);
    assert(releasing.active == hid_route::OutputRoute::kBle);
    assert(releasing.transition == hid_route::Transition::kReleasing);
    assert(releasing.generation == initial_route.generation);
    const auto public_releasing = fixture.controller.route_snapshot();
    assert(public_releasing.route.desired == hid_route::OutputRoute::kNone);
    assert(public_releasing.route.active == hid_route::OutputRoute::kBle);
    assert(public_releasing.route.transition == hid_route::Transition::kReleasing);
    assert(!public_releasing.ready);
    assert(fixture.controller.queue_ble_mouse_report(1, 1, 1, 1, 1) ==
           hid_runtime::MouseReportBeginResult::kNotReady);

    // C3-C6/C31: exact Boot values, no Report ID, exact old tuple and fresh
    // release epoch. Stack acceptance is only local acceptance, not delivery.
    assert(fixture.database.notify_calls == 2);
    assert(fixture.database.notify_history_connection[0] ==
           fixture.connection);
    assert(fixture.database.notify_history_characteristic[0] ==
           fixture.database.handles.keyboard_value);
    assert(fixture.database.notify_history_length[0] == 8);
    const std::array<std::uint8_t, 8> all_up{};
    assert(fixture.database.notify_history_payload[0] == all_up);
    assert(fixture.database.notify_history_characteristic[1] ==
           fixture.database.handles.mouse_value);
    assert(fixture.database.notify_history_length[1] == 5);
    assert(fixture.database.notify_history_payload[1] == all_up);
    const auto identity =
        fixture.controller.ble_route_release_identity_for_test();
    assert(identity.authority_epoch == initial_authority.authority_epoch);
    assert(identity.route_generation == initial_authority.route_generation);
    assert(identity.ble_generation == initial_authority.ble_generation);
    assert(identity.connection_handle == initial_authority.connection_handle);
    assert(identity.release_epoch != initial_authority.release_epoch);
    assert(fixture.ble.arm_route_release_grace_calls == 1);
    assert(fixture.ble.route_release_grace_armed);
    assert(fixture.ble.disconnect_calls == 0);

    // C17/C19: neither direct USB selection nor normal BLE delivery is
    // admitted while grace owns the old route.
    assert(fixture.controller.request_route(hid_route::OutputRoute::kUsb)
               .action_result == hid_runtime::RouteTransitionResult::kBusy);
    assert(fixture.controller.activate_ble_route_internal().action_result ==
           hid_runtime::RouteTransitionResult::kNotReady);

    // C9: deterministic expiry drives the single existing hardened disconnect
    // path; no test sleeps or wall-clock timing are involved.
    assert(fixture.controller.expire_ble_route_release_grace_for_test());
    assert(fixture.controller.process_one_for_test());
    assert(fixture.ble.disconnect_calls == 1);
    assert(fixture.ble.last_connection == fixture.connection);
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kBle);
    observe_exact_route_disconnect(fixture);
    const auto none = fixture.runtime.state_machine().route_snapshot();
    assert(none.desired == hid_route::OutputRoute::kNone);
    assert(none.active == hid_route::OutputRoute::kNone);
    assert(none.transition == hid_route::Transition::kStable);
    assert(none.generation == initial_route.generation + 1U);
    assert(!fixture.controller.route_snapshot().ready);
    assert(!fixture.ble.route_release_grace_armed);

    // C18: USB can be selected only after exact BLE Disconnect established
    // stable none. Exposure setup remains independent from route selection.
    complete_attach(fixture.runtime, fixture.usb, fixture.controller);
    mount_ready(fixture.runtime);
    assert(fixture.controller.request_route(hid_route::OutputRoute::kUsb)
               .action_result == hid_runtime::RouteTransitionResult::kAccepted);
}

void test_u74c_usb_none_ble_and_direct_switch_rejection() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    assert(controller.initialize(&runtime, &usb, &ble, &database));
    complete_attach(runtime, usb, controller);
    mount_ready(runtime);
    assert(runtime.state_machine().request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);

    // C15: direct USB -> BLE never silently changes the route enum.
    advertise_ble(runtime, usb, ble, database, controller);
    constexpr std::uint16_t connection = 121;
    assert(ble.event(hid_control_executor::BleEventKind::kConnect, connection));
    assert(controller.process_one_for_test());
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.request_route(hid_route::OutputRoute::kBle).action_result ==
           hid_runtime::RouteTransitionResult::kBusy);
    assert(runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kUsb);

    // C16: the existing USB release reaches observable stable none first,
    // after which public BLE activation is legal.
    assert(controller.request_route(hid_route::OutputRoute::kNone)
               .action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(controller.request_route(hid_route::OutputRoute::kBle).action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
}

void test_u74c_release_rejection_and_disconnect_failure_are_bounded() {
    {
        ReadyBleRouteFixture fixture(122);
        fixture.database.notify_result =
            hid_control_executor::BleNotifyBackendResult::kStackRejected;
        begin_explicit_ble_route_retirement(fixture);
        // C7: one keyboard and one mouse attempt, then grace; no retry.
        assert(fixture.database.notify_calls == 2);
        assert(fixture.ble.arm_route_release_grace_calls == 1);
        assert(fixture.controller.expire_ble_route_release_grace_for_test());
        assert(fixture.controller.process_one_for_test());
        assert(fixture.ble.disconnect_calls == 1);
    }
    {
        ReadyBleRouteFixture fixture(123);
        begin_explicit_ble_route_retirement(fixture);
        fixture.ble.disconnect_result = -77;
        assert(fixture.controller.expire_ble_route_release_grace_for_test());
        assert(fixture.controller.process_one_for_test());
        // C26: no watchdog/operation was established, so lifecycle faults and
        // the route cannot falsely publish stable none.
        assert(fixture.controller.ble_snapshot().recovery_required);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kBle);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
    {
        ReadyBleRouteFixture fixture(133);
        fixture.ble.route_release_grace_result = -66;
        begin_explicit_ble_route_retirement(fixture);
        // Timer-arm failure cannot strand releasing state: no grace owner was
        // established, so the exact hardened disconnect starts immediately.
        assert(!fixture.ble.route_release_grace_armed);
        assert(fixture.ble.disconnect_calls == 1);
        assert(fixture.ble.last_connection == fixture.connection);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
    {
        ReadyBleRouteFixture fixture(135);
        begin_explicit_ble_route_retirement(fixture);
        fixture.ble.disconnect_result = -88;
        // A stronger current-authority overflow attempts teardown while grace
        // is active. Its failed initiation owns the terminal outcome.
        for (std::size_t index = 0;
             index < hid_control_executor::Controller::kActionQueueDepth;
             ++index) {
            assert(fixture.controller.signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kEncryptionChange,
                .generation = fixture.generation - 1U,
                .connection_handle = fixture.connection,
            }));
        }
        assert(!queue_control_point(
            fixture.controller, fixture.generation, fixture.connection,
            fixture.database.handles.control_point_value, true));
        assert(fixture.controller.process_wake_cycle_for_test());
        assert(fixture.ble.disconnect_calls == 1);
        assert(fixture.controller.ble_snapshot().recovery_required);
        assert(!fixture.controller.expire_ble_route_release_grace_for_test());
        assert(fixture.ble.disconnect_calls == 1);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
    }
}

void test_u74c_grace_queue_full_has_sticky_progress() {
    ReadyBleRouteFixture fixture(124);
    begin_explicit_ble_route_retirement(fixture);
    for (std::size_t index = 0;
         index < hid_control_executor::Controller::kActionQueueDepth;
         ++index) {
        assert(fixture.controller.signal_ble_event({
            .kind = hid_control_executor::BleEventKind::kEncryptionChange,
            .generation = fixture.generation - 1U,
            .connection_handle = fixture.connection,
        }));
    }
    // C11: the timer action cannot enter the full queue, but its exact due bit
    // and independent wake drive disconnect at the next executor boundary.
    assert(fixture.controller.expire_ble_route_release_grace_for_test());
    assert(fixture.controller.executor_wake_pending_for_test());
    assert(fixture.controller.process_wake_cycle_for_test());
    assert(fixture.ble.disconnect_calls == 1);
    assert(fixture.ble.last_connection == fixture.connection);
}

void test_u74c_dropped_exact_disconnect_still_completes_route() {
    ReadyBleRouteFixture fixture(130);
    for (std::size_t index = 0;
         index < hid_control_executor::Controller::kActionQueueDepth;
         ++index) {
        assert(fixture.controller.signal_ble_event({
            .kind = hid_control_executor::BleEventKind::kEncryptionChange,
            .generation = fixture.generation - 1U,
            .connection_handle = fixture.connection,
        }));
    }
    // C12 pre-grace variant: the callback itself observed the exact physical
    // Disconnect while the route was still stable BLE. Its completion
    // evidence survives queue overflow while QueueOverflow remains the
    // stronger lifecycle diagnosis.
    assert(!fixture.ble.event(hid_control_executor::BleEventKind::kDisconnect,
                              fixture.connection));
    assert(fixture.controller.process_wake_cycle_for_test());
    assert(fixture.controller.ble_snapshot().recovery_required);
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
}

void test_u74c_spontaneous_disconnect_and_stale_grace_are_fenced() {
    ReadyBleRouteFixture fixture(125);
    begin_explicit_ble_route_retirement(fixture);
    const auto old_identity =
        fixture.controller.ble_route_release_identity_for_test();

    // C12: Disconnect before grace cancels the timer and itself proves the
    // final boundary without issuing a redundant disconnect.
    observe_exact_route_disconnect(fixture);
    assert(fixture.ble.disconnect_calls == 0);
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(!fixture.controller.signal_ble_route_release_grace(old_identity));

    // C14/C27-C29: even the same numeric handle belongs to a fresh generation;
    // reconnect/security/CCCD eligibility does not restore route authority.
    assert(fixture.ble.event(hid_control_executor::BleEventKind::kConnect,
                             fixture.connection));
    assert(fixture.controller.process_one_for_test());
    const auto generation_b = fixture.controller.ble_snapshot().generation;
    assert(generation_b != old_identity.ble_generation);
    subscribe_composite(fixture.controller, fixture.database, generation_b,
                        fixture.connection,
                        hid_control_executor::BleSubscriptionReason::kRestore);
    make_security_ready(fixture.ble);
    fixture.ble.refresh_security(fixture.connection);
    assert(fixture.controller.ble_link_ready());
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(!fixture.controller.signal_ble_route_release_grace(old_identity));
    assert(fixture.ble.disconnect_calls == 0);
}

void test_u74c_disconnect_before_first_release_wake_releases_owner() {
    ReadyBleRouteFixture fixture(134);
    assert(fixture.controller.request_route(hid_route::OutputRoute::kNone)
               .action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(fixture.controller.active_operation_for_test() ==
           ControlOperation::kRouteChange);
    // The callback arrives before the capacity-independent Stage-A wake is
    // consumed and before controller phase state has copied the operation.
    assert(fixture.ble.event(hid_control_executor::BleEventKind::kDisconnect,
                             fixture.connection));
    assert(fixture.controller.process_one_for_test());
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(fixture.controller.active_operation_for_test() ==
           ControlOperation::kNone);
    assert(fixture.ble.disconnect_calls == 0);
}

void test_u74c_disconnect_expiry_race_and_release_action_reuse() {
    ReadyBleRouteFixture fixture(126);
    begin_explicit_ble_route_retirement(fixture);
    const auto old_identity =
        fixture.controller.ble_route_release_identity_for_test();
    assert(fixture.controller.expire_ble_route_release_grace_for_test());
    hid_control_executor::Controller::Action old_expiry{};
    assert(fixture.controller.dequeue_one_for_test(old_expiry));

    // C13: exact Disconnect wins while the expired action is delayed; the
    // later action is fenced and cannot issue a second disconnect.
    observe_exact_route_disconnect(fixture);
    fixture.controller.process_for_test(old_expiry);
    assert(fixture.ble.disconnect_calls == 0);

    assert(fixture.ble.event(hid_control_executor::BleEventKind::kConnect,
                             fixture.connection));
    assert(fixture.controller.process_one_for_test());
    const auto generation_b = fixture.controller.ble_snapshot().generation;
    subscribe_composite(fixture.controller, fixture.database, generation_b,
                        fixture.connection);
    make_security_ready(fixture.ble);
    fixture.ble.refresh_security(fixture.connection);
    assert(fixture.controller.activate_ble_route_internal().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    begin_explicit_ble_route_retirement(fixture);
    const auto new_identity =
        fixture.controller.ble_route_release_identity_for_test();
    // C10/C28/C32/C33: stale action acknowledgment cannot consume the newer
    // retirement despite connection-handle reuse.
    assert(new_identity.release_epoch !=
               old_identity.release_epoch ||
           new_identity.ble_generation !=
               old_identity.ble_generation);
    fixture.controller.process_for_test(old_expiry);
    assert(fixture.ble.disconnect_calls == 0);
    assert(fixture.controller.expire_ble_route_release_grace_for_test());
    assert(fixture.controller.process_one_for_test());
    assert(fixture.ble.disconnect_calls == 1);
}

void test_u74c_release_epoch_wrap_has_exact_zero_owner() {
    ReadyBleRouteFixture fixture(136);
    fixture.runtime.state_machine().set_release_epoch_for_test(UINT32_MAX);
    fixture.controller.signal_hid_authority_change();
    assert(fixture.controller.process_wake_cycle_for_test());
    const auto identity =
        fixture.controller.ble_route_release_identity_for_test();
    assert(identity.release_epoch == 0);
    assert(fixture.controller.expire_ble_route_release_grace_for_test());
    assert(fixture.controller.process_one_for_test());
    assert(fixture.ble.disconnect_calls == 1);
    observe_exact_route_disconnect(fixture);
    assert(fixture.runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
}

void test_u74c_lease_overflow_and_disable_retirement_paths() {
    {
        ReadyBleRouteFixture fixture(127);
        // C8/C29 plus lease boundary: the existing 5000 ms authority callback
        // publishes release work; it cannot leave BLE normal authority alive.
        fixture.runtime.state_machine().request_release_all();
        fixture.controller.signal_hid_authority_change();
        assert(fixture.controller.process_wake_cycle_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        const std::array<std::uint8_t, 6> no_keys{};
        assert(fixture.controller.queue_ble_keyboard_report(
                   0, no_keys) ==
               hid_runtime::KeyboardReportBeginResult::kNotReady);
    }
    {
        ReadyBleRouteFixture fixture(128);
        begin_explicit_ble_route_retirement(fixture);
        for (std::size_t index = 0;
             index < hid_control_executor::Controller::kActionQueueDepth;
             ++index) {
            assert(fixture.controller.signal_ble_event({
                .kind = hid_control_executor::BleEventKind::kEncryptionChange,
                .generation = fixture.generation - 1U,
                .connection_handle = fixture.connection,
            }));
        }
        assert(!queue_control_point(
            fixture.controller, fixture.generation, fixture.connection,
            fixture.database.handles.control_point_value, true));
        assert(fixture.controller.process_wake_cycle_for_test());
        // C25: generic overflow truth wins and initiates bounded teardown; it
        // does not overwrite the retained exact route-release identity.
        assert(fixture.controller.ble_snapshot().recovery_required);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
    }
    {
        ReadyBleRouteFixture fixture(129);
        // Exposure disable remains independent but cannot bypass the route's
        // observable stable-none boundary.
        assert(fixture.controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kBusy);
        begin_explicit_ble_route_retirement(fixture);
        const auto releasing =
            fixture.runtime.state_machine().route_snapshot();
        assert(releasing.active == hid_route::OutputRoute::kBle);
        assert(releasing.transition == hid_route::Transition::kReleasing);
        assert(fixture.controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kBusy);
        assert(fixture.runtime.state_machine().route_snapshot().transition ==
               hid_route::Transition::kReleasing);
        assert(fixture.ble.disconnect_calls == 0);
        assert(fixture.controller.expire_ble_route_release_grace_for_test());
        assert(fixture.controller.process_one_for_test());
        assert(fixture.ble.disconnect_calls == 1);
        observe_exact_route_disconnect(fixture);
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
        assert(fixture.controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(fixture.controller.process_one_for_test());
        assert(fixture.controller.ble_snapshot().observed ==
               ble_lifecycle::ObservedState::kIdle);
    }
    {
        ReadyBleRouteFixture fixture(131);
        // A host reset is itself exact proof that the old controller-owned
        // connection is gone; it need not wait for an impossible Disconnect.
        assert(fixture.ble.event(
            hid_control_executor::BleEventKind::kReset,
            ble_lifecycle::kNoConnection, -9));
        assert(fixture.controller.process_one_for_test());
        assert(fixture.runtime.state_machine().route_snapshot().active ==
               hid_route::OutputRoute::kNone);
        assert(fixture.ble.disconnect_calls == 0);
    }
}

void test_u74c_ble_completion_preserves_independent_usb_uncertainty() {
    ReadyBleRouteFixture fixture(132);
    const std::array<std::uint8_t, 6> held_key{0x73, 0, 0, 0, 0, 0};
    assert(fixture.controller.queue_ble_keyboard_report(0, held_key) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(fixture.controller.process_one_for_test());
    fixture.runtime.state_machine().finalize_keyboard_report();

    assert(action(fixture.controller.request_attach()) ==
           usb_lifecycle::TransitionResult::kAccepted);
    assert(fixture.controller.process_one_for_test());
    fixture.runtime.state_machine().on_mount();
    fixture.controller.signal_hid_authority_change();
    assert(fixture.controller.process_wake_cycle_for_test());
    assert(fixture.runtime.state_machine().route_snapshot().transition ==
           hid_route::Transition::kReleasing);

    // An independent USB host loss becomes uncertain while BLE is retiring.
    // Exact BLE completion must not erase that stronger USB safety barrier.
    fixture.runtime.state_machine().on_unmount();
    fixture.controller.signal_hid_authority_change();
    assert(fixture.controller.process_wake_cycle_for_test());
    observe_exact_route_disconnect(fixture);
    const auto usb_lifecycle =
        fixture.runtime.state_machine().usb_lifecycle_snapshot();
    assert(usb_lifecycle.host_release_uncertain);
    assert(usb_lifecycle.safety_pending);
    assert(fixture.runtime.state_machine().safety_required(
        hid_runtime::Interface::kKeyboard));
}

void assert_fatal_storage_terminal_state(
    hid_control_executor::Controller &controller, FakeBleBackend &ble,
    std::uint16_t handle, int advertising_calls_before_disconnect) {
    const auto failed = controller.ble_snapshot();
    assert(failed.desired == ble_lifecycle::DesiredExposure::kExposed);
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required);
    assert(!failed.advertising && !failed.connected);
    assert(failed.last_error.present);
    assert(failed.last_error.operation == ble_lifecycle::Operation::kRuntime);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kIdle);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStorage);
    assert(controller.pairing_snapshot().pairing_id == 0);
    assert(ble.timer_pairing_id == 0);
    assert(controller.pairing_mailbox_zero_for_test());
    assert(!ble.security_ready_for_hid(failed.generation, handle));
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(ble.disconnect_calls == 1);

    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, handle));
    assert(controller.process_one_for_test());
    const auto after_disconnect = controller.ble_snapshot();
    assert(after_disconnect.desired == ble_lifecycle::DesiredExposure::kExposed);
    assert(after_disconnect.observed == ble_lifecycle::ObservedState::kFault);
    assert(after_disconnect.recovery_required);
    assert(!after_disconnect.advertising && !after_disconnect.connected);
    assert(ble.advertising_calls == advertising_calls_before_disconnect);

    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kBusy);
    const auto after_disable = controller.ble_snapshot();
    assert(after_disable.observed == ble_lifecycle::ObservedState::kFault);
    assert(after_disable.recovery_required);
}

void run_persisted_bond_reread_failure(
    ble_security::PersistedSecurityEvidence persisted,
    std::uint16_t handle) {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    connect_ble(runtime, usb, ble, database, controller, handle);
    assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                     handle, 1));
    assert(controller.process_one_for_test());
    const auto pending = controller.pairing_snapshot();
    const std::array<char, 6> secret = {'0', '0', '0', '1', '2', '3'};
    assert(controller.respond_to_pairing(pending.generation, handle,
                                         pending.pairing_id, secret) ==
           ble_pairing::RespondResult::kAccepted);
    make_security_ready(ble);
    ble.security_persisted = persisted;
    const int advertising_calls = ble.advertising_calls;
    assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete,
                     handle, 0));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
    assert(!controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kSecuring);
    assert(ble.disconnect_calls == 0);
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     handle, 0));
    assert(controller.process_one_for_test());
    assert_fatal_storage_terminal_state(controller, ble, handle,
                                        advertising_calls);
}

void test_missing_our_sec_is_fatal_storage_failure() {
    run_persisted_bond_reread_failure(
        {.our = {}, .peer = valid_security_record()}, 81);
}

void test_missing_peer_sec_is_fatal_storage_failure() {
    run_persisted_bond_reread_failure(
        {.our = valid_security_record(), .peer = {}}, 82);
}

void test_persisted_bond_reread_mismatch_is_fatal_storage_failure() {
    auto mismatched = valid_security_record();
    mismatched.identity_matches = false;
    run_persisted_bond_reread_failure(
        {.our = valid_security_record(), .peer = mismatched}, 83);
}

void test_low_level_storage_error_is_global_even_with_stale_identity() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 84);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                         84, 1));
        assert(controller.process_one_for_test());
        const int advertising_calls = ble.advertising_calls;
        assert(ble.event(hid_control_executor::BleEventKind::kStorageFailure,
                         84));
        assert(controller.process_one_for_test());
        assert_fatal_storage_terminal_state(controller, ble, 84,
                                            advertising_calls);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 85);
        const auto generation = controller.ble_snapshot().generation;
        const int advertising_calls = ble.advertising_calls;
        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kStorageFailure,
            generation - 1, 85));
        assert(controller.process_one_for_test());
        assert_fatal_storage_terminal_state(controller, ble, 85,
                                            advertising_calls);
    }
}

void test_store_full_remains_recoverable_and_readvertises() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    connect_ble(runtime, usb, ble, database, controller, 86);
    assert(ble.event(hid_control_executor::BleEventKind::kStoreFull, 86));
    assert(controller.process_one_for_test());
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStoreFull);
    assert(!controller.ble_snapshot().recovery_required);
    assert(!ble.persistent_store_failure_observed());
    assert(ble.disconnect_calls == 1);
    const int advertising_calls = ble.advertising_calls;
    assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 86));
    assert(controller.process_one_for_test());
    const auto recovered = controller.ble_snapshot();
    assert(recovered.observed == ble_lifecycle::ObservedState::kAdvertising);
    assert(recovered.advertising && !recovered.connected);
    assert(!recovered.recovery_required);
    assert(ble.advertising_calls == advertising_calls + 1);
}

void test_pairing_input_response_and_initiation() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    connect_ble(runtime, usb, ble, database, controller);
    const auto generation = controller.ble_snapshot().generation;
    assert(ble.initiate_security_calls == 1);
    assert(ble.event(hid_control_executor::BleEventKind::kConnect, 23));
    assert(controller.process_one_for_test());
    assert(ble.initiate_security_calls == 1);

    assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 23, 1));
    assert(controller.process_one_for_test());
    const auto waiting = controller.pairing_snapshot();
    assert(waiting.live_state == ble_pairing::LiveState::kWaitingInput);
    assert(waiting.pairing_id != 0);
    assert(ble.arm_pairing_timeout_calls == 1);
    assert(ble.timer_generation == generation && ble.timer_connection == 23);
    assert(ble.timer_pairing_id == waiting.pairing_id);

    assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 23, 1));
    assert(controller.process_one_for_test());
    assert(controller.pairing_snapshot().pairing_id == waiting.pairing_id);
    assert(ble.arm_pairing_timeout_calls == 1);

    const std::array<char, 6> malformed = {'0', '1', '2', 'x', '4', '5'};
    assert(controller.respond_to_pairing(generation, 23, waiting.pairing_id,
                                         malformed) ==
           ble_pairing::RespondResult::kInvalidSecret);
    assert(ble.inject_calls == 0);
    const std::array<char, 6> secret = {'0', '1', '2', '3', '4', '5'};
    assert(controller.respond_to_pairing(generation - 1, 23,
                                         waiting.pairing_id, secret) ==
           ble_pairing::RespondResult::kStaleGeneration);
    assert(controller.respond_to_pairing(generation, 24, waiting.pairing_id,
                                         secret) ==
           ble_pairing::RespondResult::kStaleConnection);
    assert(controller.respond_to_pairing(generation, 23,
                                         waiting.pairing_id + 1, secret) ==
           ble_pairing::RespondResult::kStalePairingId);
    assert(controller.respond_to_pairing(generation, 23, waiting.pairing_id,
                                         secret) ==
           ble_pairing::RespondResult::kAccepted);
    assert(ble.inject_calls == 1 && ble.last_injected_value == 12345);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kSecuring);
    assert(controller.pairing_snapshot().pairing_id == 0);
    assert(controller.respond_to_pairing(generation, 23, waiting.pairing_id,
                                         secret) ==
           ble_pairing::RespondResult::kNotPending);
    assert(ble.inject_calls == 1);
}

void test_public_pairing_rpc_mailbox_status_and_races() {
    const std::array<char, 6> secret = {'0', '0', '0', '1', '2', '3'};
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 71);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 71, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        const auto status = controller.request_pairing_status();
        assert(status.generation == pending.generation && status.connected);
        assert(status.pairing.live_state == ble_pairing::LiveState::kWaitingInput);
        assert(status.remaining_ms == ble_pairing::kInputTimeoutMs);
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kAccepted);
        assert(ble.inject_calls == 1 && ble.last_injected_value == 123);
        assert(controller.pairing_mailbox_zero_for_test());
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kNotPending);
        assert(ble.inject_calls == 1);
        assert(controller.pairing_mailbox_zero_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 72);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 72, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        ble.now_us = static_cast<std::uint64_t>(ble_pairing::kInputTimeoutMs) * 1000U;
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kNotPending);
        assert(ble.inject_calls == 0 && ble.disconnect_calls == 1);
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kTimeout);
        assert(controller.pairing_mailbox_zero_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 73);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 73, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 73));
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kNotPending);
        assert(ble.inject_calls == 0);
        assert(controller.pairing_mailbox_zero_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 74);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 74, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        ble.inject_result = -1;
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kInjectionFailed);
        assert(ble.inject_calls == 1 && ble.disconnect_calls == 1);
        assert(controller.pairing_mailbox_zero_for_test());
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 75);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 75, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        assert(controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.request_pairing_response(pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kNotPending);
        assert(ble.inject_calls == 0);
        assert(controller.pairing_mailbox_zero_for_test());
    }
}

void test_security_event_ordering_and_existing_bond() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    connect_ble(runtime, usb, ble, database, controller, 31);
    const auto generation = controller.ble_snapshot().generation;
    assert(controller.pairing_snapshot().pairing_id == 0);

    assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete, 31, 0));
    assert(controller.process_one_for_test());
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kSecuring);
    make_security_ready(ble, false);
    assert(ble.event(hid_control_executor::BleEventKind::kIdentityResolved, 31));
    assert(controller.process_one_for_test());
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kSucceeded);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kIdle);
    assert(ble.security_ready_for_hid(generation, 31));

    const int refreshes = ble.refresh_security_calls;
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kEncryptionChange,
        generation - 1, 31, 0));
    assert(controller.process_one_for_test());
    assert(ble.refresh_security_calls == refreshes);
}

void test_pairing_completion_waits_for_post_persistence_event() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t handle = 32;
    connect_ble(runtime, usb, ble, database, controller, handle);

    assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                     handle, 1));
    assert(controller.process_one_for_test());
    const auto pending = controller.pairing_snapshot();
    const std::array<char, 6> secret = {'0', '0', '0', '1', '2', '3'};
    assert(controller.respond_to_pairing(pending.generation, handle,
                                         pending.pairing_id, secret) ==
           ble_pairing::RespondResult::kAccepted);

    // NimBLE exposes the successful link before it persists OUR_SEC and
    // PEER_SEC. Pairing Complete and the in-persistence Identity Resolved
    // event must therefore tolerate this temporary absence.
    make_security_ready(ble);
    ble.security_persisted = {};
    assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete,
                     handle, 0));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
    assert(!controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kSecuring);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kNone);
    assert(ble.disconnect_calls == 0);

    assert(ble.event(hid_control_executor::BleEventKind::kIdentityResolved,
                     handle, 0));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().connected);
    assert(!controller.ble_snapshot().recovery_required);
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kSecuring);
    assert(ble.disconnect_calls == 0);

    // Encryption Change is queued after the synchronous bond persistence
    // attempts. The same connection becomes ready once the complete pair is
    // observable.
    ble.security_persisted = {.our = valid_security_record(),
                              .peer = valid_security_record()};
    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     handle, 0));
    assert(controller.process_one_for_test());
    assert(controller.pairing_snapshot().live_state ==
           ble_pairing::LiveState::kIdle);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kSucceeded);
    assert(ble.security_ready_for_hid(controller.ble_snapshot().generation,
                                     handle));
    assert(ble.disconnect_calls == 0);
}

void test_timeout_repeat_store_and_disconnect_results() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 41);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 41, 1));
        assert(controller.process_one_for_test());
        const auto pending = controller.pairing_snapshot();
        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kPairingTimeout,
            pending.generation, 41, 0));
        // Wrong pairing ID is stale and ignored.
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().live_state ==
               ble_pairing::LiveState::kWaitingInput);
        assert(ble.sink->signal_ble_event({
            .kind = hid_control_executor::BleEventKind::kPairingTimeout,
            .generation = pending.generation,
            .connection_handle = 41,
            .status = 0,
            .pairing_id = pending.pairing_id,
        }));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kTimeout);
        assert(ble.disconnect_calls == 1);
        assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 41));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kTimeout);
    }
    for (const auto &item : {
             std::pair{hid_control_executor::BleEventKind::kRepeatPairing,
                       ble_pairing::LastResult::kRepeatPairing},
             std::pair{hid_control_executor::BleEventKind::kStoreFull,
                       ble_pairing::LastResult::kStoreFull}}) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 42);
        assert(ble.event(item.first, 42));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result == item.second);
        assert(ble.disconnect_calls == 1);
        assert(!controller.ble_snapshot().recovery_required);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 43);
        assert(ble.event(hid_control_executor::BleEventKind::kStorageFailure, 43));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kStorage);
        assert(controller.ble_snapshot().recovery_required);
    }
}

void test_store_full_disconnect_failure_terminalizes_recovery() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 109;
    constexpr std::int32_t disconnect_error = -91;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    subscribe_composite(controller, database, generation, connection);
    make_security_ready(ble);
    ble.refresh_security(connection);
    assert(controller.ble_link_ready());
    ble.disconnect_result = disconnect_error;

    assert(ble.event(hid_control_executor::BleEventKind::kStoreFull,
                     connection, -90));
    assert(controller.process_one_for_test());

    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.connected && !failed.advertising);
    assert(failed.last_error.present &&
           failed.last_error.operation == ble_lifecycle::Operation::kRuntime &&
           failed.last_error.code == disconnect_error);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kStoreFull);
    assert(!ble.persistent_store_failure_observed());
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());
    assert(ble.disconnect_calls == 1);
}

void test_pairing_timeout_disconnect_failure_terminalizes_recovery() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 110;
    constexpr std::int32_t disconnect_error = -92;
    connect_ble(runtime, usb, ble, database, controller, connection);
    assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                     connection, 1));
    assert(controller.process_one_for_test());
    const auto pending = controller.pairing_snapshot();
    ble.disconnect_result = disconnect_error;

    assert(ble.sink->signal_ble_event({
        .kind = hid_control_executor::BleEventKind::kPairingTimeout,
        .generation = pending.generation,
        .connection_handle = connection,
        .pairing_id = pending.pairing_id,
    }));
    assert(controller.process_one_for_test());

    const auto failed = controller.ble_snapshot();
    assert(failed.observed == ble_lifecycle::ObservedState::kFault);
    assert(failed.recovery_required && !failed.connected);
    assert(failed.last_error.present &&
           failed.last_error.code == disconnect_error);
    assert(controller.pairing_snapshot().last_result ==
           ble_pairing::LastResult::kTimeout);
    assert(ble.disconnect_calls == 1);
}

void test_smp_and_repeat_pairing_disconnect_failures_terminalize_recovery() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::int32_t disconnect_error = -93;
        advertise_ble(runtime, usb, ble, database, controller);
        ble.initiate_security_result = -94;
        ble.disconnect_result = disconnect_error;
        assert(ble.event(hid_control_executor::BleEventKind::kConnect, 111));
        assert(controller.process_one_for_test());

        const auto failed = controller.ble_snapshot();
        assert(failed.observed == ble_lifecycle::ObservedState::kFault);
        assert(failed.recovery_required && !failed.connected);
        assert(failed.last_error.present &&
               failed.last_error.code == disconnect_error);
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kSmpFailed);
        assert(ble.disconnect_calls == 1);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::int32_t disconnect_error = -95;
        connect_ble(runtime, usb, ble, database, controller, 112);
        ble.disconnect_result = disconnect_error;

        assert(ble.event(hid_control_executor::BleEventKind::kRepeatPairing,
                         112));
        assert(controller.process_one_for_test());
        const auto failed = controller.ble_snapshot();
        assert(failed.observed == ble_lifecycle::ObservedState::kFault);
        assert(failed.recovery_required && !failed.connected);
        assert(failed.last_error.present &&
               failed.last_error.code == disconnect_error);
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kRepeatPairing);
        assert(ble.disconnect_calls == 1);
    }
}

void test_security_teardown_already_disconnected_reconciles_exact_peer() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 113;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto generation = controller.ble_snapshot().generation;
    ble.disconnect_result = ble.already_disconnected_result;
    ble.bond_list_result = {
        .kind = hid_control_executor::BleBondListResultKind::kSuccess,
        .healthy = true};

    assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                     connection, -1));
    assert(controller.process_one_for_test());

    const auto disconnected = controller.ble_snapshot();
    assert(disconnected.generation != generation);
    assert(disconnected.observed ==
           ble_lifecycle::ObservedState::kAdvertising);
    assert(disconnected.advertising && !disconnected.connected);
    assert(!disconnected.recovery_required && !disconnected.last_error.present);
    const auto pairing = controller.pairing_snapshot();
    assert(pairing.live_state == ble_pairing::LiveState::kIdle);
    assert(pairing.last_result == ble_pairing::LastResult::kSmpFailed);
    assert(pairing.connection_handle == ble_lifecycle::kNoConnection);
    assert(!controller.ble_hid_peer_snapshot().active);
    assert(!controller.ble_link_ready());
    assert(runtime.state_machine().route_snapshot().active ==
           hid_route::OutputRoute::kNone);
    assert(ble.disconnect_calls == 1);
    assert(ble.advertising_calls == 2);  // Existing reconnect policy only.

    // A stale-key failure that ended in ENOTCONN is not a storage fault.
    assert(controller.request_bond_list().kind ==
           hid_control_executor::BleBondListResultKind::kSuccess);
    assert(ble.bond_list_calls == 1);

    // Explicit disable still reaches the hidden terminal idle state without
    // requiring a synthetic or late physical Disconnect callback.
    assert(controller.request_ble_disable().action_result ==
           ble_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    const auto idle = controller.ble_snapshot();
    assert(idle.observed == ble_lifecycle::ObservedState::kIdle);
    assert(idle.desired == ble_lifecycle::DesiredExposure::kHidden);
    assert(!idle.connected && !idle.advertising && !idle.recovery_required);
}

void test_security_teardown_normal_and_failed_disconnect_results() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::uint16_t connection = 114;
        connect_ble(runtime, usb, ble, database, controller, connection);
        const auto generation = controller.ble_snapshot().generation;

        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection, -1));
        assert(controller.process_one_for_test());
        assert(controller.ble_snapshot().generation == generation);
        assert(controller.ble_snapshot().connected);
        assert(!controller.ble_snapshot().recovery_required);
        assert(ble.disconnect_calls == 1);

        assert(ble.event_for_generation(
            hid_control_executor::BleEventKind::kDisconnect,
            generation, connection));
        assert(controller.process_one_for_test());
        assert(!controller.ble_snapshot().connected);
        assert(!controller.ble_snapshot().recovery_required);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        constexpr std::uint16_t connection = 115;
        connect_ble(runtime, usb, ble, database, controller, connection);
        ble.disconnect_result = -96;

        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection, -1));
        assert(controller.process_one_for_test());
        const auto failed = controller.ble_snapshot();
        assert(failed.observed == ble_lifecycle::ObservedState::kFault);
        assert(failed.recovery_required && !failed.connected);
        assert(failed.last_error.present && failed.last_error.code == -96);
        assert(!ble.persistent_store_failure_observed());

        // Generic lifecycle recovery is not mislabeled as persistent Storage.
        assert(controller.request_bond_list().kind ==
               hid_control_executor::BleBondListResultKind::kNotReady);
        assert(controller.request_bond_remove(executor_bond_id('e')).kind ==
               hid_control_executor::BleBondRemoveResultKind::kBusy);
    }
}

void test_already_disconnected_identity_late_event_and_handle_reuse_fences() {
    hid_runtime::Runtime runtime;
    FakeBackend usb;
    FakeBleBackend ble;
    FakeBleDatabase database;
    hid_control_executor::Controller controller;
    constexpr std::uint16_t connection = 116;
    connect_ble(runtime, usb, ble, database, controller, connection);
    const auto old_generation = controller.ble_snapshot().generation;

    // Neither an old generation nor a different handle can retire the peer.
    assert(!controller.reconcile_security_disconnect_absent_for_test(
        old_generation - 1U, connection));
    assert(!controller.reconcile_security_disconnect_absent_for_test(
        old_generation, connection + 1U));
    assert(controller.ble_snapshot().generation == old_generation);
    assert(controller.ble_snapshot().connected);

    assert(controller.reconcile_security_disconnect_absent_for_test(
        old_generation, connection));
    const auto terminal = controller.ble_snapshot();
    assert(terminal.generation != old_generation);
    assert(!terminal.connected && !terminal.recovery_required);
    const int advertising_after_terminal = ble.advertising_calls;

    // A callback queued before the synchronous absence result is now stale.
    assert(ble.event_for_generation(
        hid_control_executor::BleEventKind::kDisconnect,
        old_generation, connection));
    assert(controller.process_one_for_test());
    assert(controller.ble_snapshot().generation == terminal.generation);
    assert(!controller.ble_snapshot().connected);
    assert(ble.advertising_calls == advertising_after_terminal);

    // The numeric handle may be reused only under the newer generation. Old
    // teardown evidence cannot touch that adopted peer.
    assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                     connection));
    assert(controller.process_one_for_test());
    const auto reused = controller.ble_snapshot();
    assert(reused.generation == terminal.generation && reused.connected);
    assert(!controller.reconcile_security_disconnect_absent_for_test(
        old_generation, connection));
    assert(controller.ble_snapshot().generation == reused.generation);
    assert(controller.ble_snapshot().connected);
}

void test_already_disconnected_completes_releasing_ble_route() {
    ReadyBleRouteFixture fixture(117);
    fixture.ble.disconnect_result = fixture.ble.already_disconnected_result;

    assert(fixture.ble.event(
        hid_control_executor::BleEventKind::kEncryptionChange,
        fixture.connection, -1));
    assert(fixture.controller.process_one_for_test());

    const auto route = fixture.runtime.state_machine().route_snapshot();
    assert(route.desired == hid_route::OutputRoute::kNone);
    assert(route.active == hid_route::OutputRoute::kNone);
    assert(route.transition == hid_route::Transition::kStable);
    assert(!fixture.controller.ble_snapshot().recovery_required);
    assert(!fixture.controller.ble_snapshot().connected);
}

void test_queue_burst_overflow_and_id_wrap_fail_closed() {
    static_assert(hid_control_executor::Controller::kActionQueueDepth == 12);
    static_assert(sizeof(hid_control_executor::BleHidPeerSnapshot) == 16);
    {
        // One control action plus a bounded fresh-security burst can exceed
        // the old depth eight before the serialized task gets CPU time.
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &usb, &ble, &database));
        assert(controller.request_ble_enable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        assert(ble.event(hid_control_executor::BleEventKind::kSync));
        assert(controller.process_one_for_test());
        assert(action(controller.request_attach()) ==
               usb_lifecycle::TransitionResult::kAccepted);
        constexpr std::uint16_t connection = 49;
        assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                         connection, 1));
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kIdentityResolved,
                         connection));
        assert(queue_subscription(
            controller, ble.active_generation, connection,
            hid_control_executor::BleHidInterface::kKeyboard,
            database.handles.keyboard_value, true));
        assert(queue_subscription(
            controller, ble.active_generation, connection,
            hid_control_executor::BleHidInterface::kMouse,
            database.handles.mouse_value, true));
        assert(ble.event(hid_control_executor::BleEventKind::kStoreFull,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kPairingTimeout,
                         connection));
    }
    {
        // Bond restore: one overlapping control action, connect/security,
        // identity, two CCCD restores, and bounded store/timer callbacks.
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &usb, &ble, &database));
        assert(controller.request_ble_enable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.process_one_for_test());
        assert(ble.event(hid_control_executor::BleEventKind::kSync));
        assert(controller.process_one_for_test());
        assert(action(controller.request_attach()) ==
               usb_lifecycle::TransitionResult::kAccepted);
        constexpr std::uint16_t connection = 48;
        assert(ble.event(hid_control_executor::BleEventKind::kConnect,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kIdentityResolved,
                         connection));
        assert(queue_subscription(
            controller, ble.active_generation, connection,
            hid_control_executor::BleHidInterface::kKeyboard,
            database.handles.keyboard_value, true,
            hid_control_executor::BleSubscriptionReason::kRestore));
        assert(queue_subscription(
            controller, ble.active_generation, connection,
            hid_control_executor::BleHidInterface::kMouse,
            database.handles.mouse_value, true,
            hid_control_executor::BleSubscriptionReason::kRestore));
        assert(ble.event(hid_control_executor::BleEventKind::kStoreFull,
                         connection));
        assert(ble.event(hid_control_executor::BleEventKind::kPairingTimeout,
                         connection));
    }
    {
        // Disconnect: both TERM callbacks, disconnect, and one overlapping
        // control action plus a bounded timer callback all fit together.
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 47);
        assert(action(controller.request_attach()) ==
               usb_lifecycle::TransitionResult::kAccepted);
        assert(queue_subscription(
            controller, ble.active_generation, 47,
            hid_control_executor::BleHidInterface::kKeyboard,
            database.handles.keyboard_value, false,
            hid_control_executor::BleSubscriptionReason::kTerm));
        assert(queue_subscription(
            controller, ble.active_generation, 47,
            hid_control_executor::BleHidInterface::kMouse,
            database.handles.mouse_value, false,
            hid_control_executor::BleSubscriptionReason::kTerm));
        assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 47));
        assert(ble.event(hid_control_executor::BleEventKind::kPairingTimeout,
                         47));
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 50);
        assert(action(controller.request_attach()) ==
               usb_lifecycle::TransitionResult::kAccepted);
        for (std::size_t index = 0;
             index < hid_control_executor::Controller::kActionQueueDepth - 1;
             ++index) {
            assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                             50, 1));
        }
        assert(!ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                          50, 1));
        assert(controller.process_one_for_test());
        assert(usb.install_calls == 1);
        assert(controller.active_operation_for_test() == ControlOperation::kNone);
        assert(controller.ble_snapshot().recovery_required);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 51);
        for (std::size_t index = 0;
             index < hid_control_executor::Controller::kActionQueueDepth;
             ++index) {
            assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                             51, 1));
        }
        assert(!ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                          51, 1));
        assert(controller.process_one_for_test());
        assert(controller.ble_snapshot().recovery_required);
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kQueueOverflow);
        assert(ble.disconnect_calls == 1);
        assert(!controller.ble_hid_peer_snapshot().active);
        const hid_control_executor::BleHidWorkIdentity retired{
            .generation = ble.active_generation,
            .connection_handle = 51,
            .characteristic_handle = database.handles.keyboard_value,
        };
        assert(controller.submit_ble_keyboard(
                   retired, hid_control_executor::kBleKeyboardAllUp) ==
               hid_control_executor::BleHidSubmitResult::kStale);
        assert(database.notify_calls == 0);
        while (controller.process_one_for_test()) {
        }
        assert(queue_subscription(
            controller, retired.generation, retired.connection_handle,
            hid_control_executor::BleHidInterface::kKeyboard,
            retired.characteristic_handle, true));
        assert(queue_control_point(
            controller, retired.generation, retired.connection_handle,
            database.handles.control_point_value, false));
        assert(controller.process_one_for_test());
        assert(controller.process_one_for_test());
        assert(!controller.ble_hid_peer_snapshot().active);
        assert(!controller.ble_link_ready());
        assert(database.notify_calls == 0);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 52);
        controller.set_next_pairing_id_for_test(
            std::numeric_limits<std::uint32_t>::max());
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                         52, 1));
        assert(controller.process_one_for_test());
        auto pending = controller.pairing_snapshot();
        assert(pending.pairing_id == std::numeric_limits<std::uint32_t>::max());
        const std::array<char, 6> secret = {'9', '9', '9', '9', '9', '9'};
        assert(controller.respond_to_pairing(pending.generation, 52,
                                             pending.pairing_id, secret) ==
               ble_pairing::RespondResult::kAccepted);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                         52, 1));
        assert(controller.process_one_for_test());
        assert(controller.ble_snapshot().recovery_required);
        assert(controller.pairing_snapshot().id_exhausted);
    }
}

void test_policy_persistence_disconnect_disable_and_bounded_burst() {
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 61);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 61, 0));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kSecurityPolicy);
        assert(ble.disconnect_calls == 1);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 62);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 62, 1));
        assert(controller.process_one_for_test());
        assert(ble.event(hid_control_executor::BleEventKind::kDisconnect, 62));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kPeerDisconnected);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 63);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction, 63, 1));
        assert(controller.process_one_for_test());
        const auto pairing_before_disable = controller.pairing_snapshot();
        assert(controller.request_ble_disable().action_result ==
               ble_lifecycle::TransitionResult::kAccepted);
        assert(controller.pairing_snapshot().live_state ==
               pairing_before_disable.live_state);
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kNone);
        assert(controller.pairing_snapshot().live_state ==
               ble_pairing::LiveState::kIdle);
    }
    for (const bool downgraded : {false, true}) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller,
                    downgraded ? 65 : 64);
        ble.security_link = {.encrypted = true,
                             .authenticated = !downgraded,
                             .nimble_bonded = true,
                             .secure_connections = true,
                             .identity_resolved = true,
                             .key_size = static_cast<std::uint8_t>(
                                 downgraded ? 15 : 16)};
        // Missing persisted records is storage failure when link policy is
        // valid; downgraded link evidence is a security-policy failure.
        const auto handle = static_cast<std::uint16_t>(downgraded ? 65 : 64);
        assert(ble.event(hid_control_executor::BleEventKind::kPairingComplete,
                         handle, 0));
        assert(controller.process_one_for_test());
        assert(controller.ble_snapshot().connected);
        assert(ble.disconnect_calls == 0);
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         handle, 0));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               (downgraded ? ble_pairing::LastResult::kSecurityPolicy
                           : ble_pairing::LastResult::kStorage));
        assert(ble.disconnect_calls == 1);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 66);
        const auto generation = controller.ble_snapshot().generation;
        for (int index = 0; index < 8; ++index) {
            assert(ble.event_for_generation(
                hid_control_executor::BleEventKind::kEncryptionChange,
                generation - 1, 66, 0));
        }
        for (int index = 0; index < 8; ++index) {
            assert(controller.process_one_for_test());
        }
        assert(!controller.ble_snapshot().recovery_required);
        assert(controller.ble_snapshot().connected);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 67);
        make_security_ready(ble);
        assert(ble.event(hid_control_executor::BleEventKind::kEncryptionChange,
                         67, 0));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().last_result ==
               ble_pairing::LastResult::kSucceeded);
        assert(controller.pairing_snapshot().pairing_id == 0);
    }
}

void test_bond_administration_serialization_and_safety_policy() {
    const auto id = executor_bond_id('a');
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &usb, &ble, &database));
        assert(controller.request_bond_list().kind ==
               hid_control_executor::BleBondListResultKind::kNotReady);
        assert(controller.request_bond_remove(id).kind ==
               hid_control_executor::BleBondRemoveResultKind::kNotReady);
        assert(ble.bond_list_calls == 0 && ble.bond_remove_calls == 0);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        advertise_ble(runtime, usb, ble, database, controller);
        ble.bond_list_result.count = 1;
        ble.bond_list_result.available = 2;
        ble.bond_list_result.bonds[0] = {
            .bond_id = id, .our_sec = true, .peer_sec = true,
            .verified = true};
        const auto listed = controller.request_bond_list();
        assert(listed.kind ==
               hid_control_executor::BleBondListResultKind::kSuccess);
        assert(listed.count == 1 && ble.bond_list_calls == 1);
        // Advertising is deliberately not an eligible destructive boundary:
        // a physical connection could otherwise race the store mutation.
        assert(controller.request_bond_remove(id).kind ==
               hid_control_executor::BleBondRemoveResultKind::kBusy);
        assert(ble.bond_remove_calls == 0);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        connect_ble(runtime, usb, ble, database, controller, 71);
        // Read-only inventory remains available during a coherent connection.
        ble.bond_list_result = {
            .kind = hid_control_executor::BleBondListResultKind::kSuccess,
            .healthy = true};
        assert(controller.request_bond_list().kind ==
               hid_control_executor::BleBondListResultKind::kSuccess);
        assert(ble.event(hid_control_executor::BleEventKind::kPasskeyAction,
                         71, 1));
        assert(controller.process_one_for_test());
        assert(controller.pairing_snapshot().pairing_active);
        assert(controller.request_bond_remove(id).kind ==
               hid_control_executor::BleBondRemoveResultKind::kBusy);
        assert(ble.bond_remove_calls == 0);
    }
    for (const bool releasing : {false, true}) {
        ReadyBleRouteFixture fixture(releasing ? 73 : 72);
        if (releasing) {
            assert(fixture.controller.request_route(hid_route::OutputRoute::kNone)
                       .action_result ==
                   hid_runtime::RouteTransitionResult::kAccepted);
            assert(fixture.runtime.state_machine().route_snapshot().transition ==
                   hid_route::Transition::kReleasing);
        }
        assert(fixture.controller.request_bond_remove(id).kind ==
               hid_control_executor::BleBondRemoveResultKind::kBusy);
        assert(fixture.ble.bond_remove_calls == 0);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        assert(controller.initialize(&runtime, &usb, &ble, &database));
        complete_attach(runtime, usb, controller);
        mount_ready(runtime);
        assert(controller.request_route(hid_route::OutputRoute::kUsb).action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
        hide_ble(runtime, usb, ble, database, controller);
        const auto route_before = runtime.state_machine().route_snapshot();
        const auto authority_before = runtime.state_machine().authority_epoch();
        const auto keyboard_before = runtime.state_machine().keyboard_state();
        const auto mouse_before = runtime.state_machine().mouse_state();
        ble.bond_remove_result = {
            .kind = hid_control_executor::BleBondRemoveResultKind::kSuccess,
            .remaining = 2};
        const auto removed = controller.request_bond_remove(id);
        assert(removed.kind ==
               hid_control_executor::BleBondRemoveResultKind::kSuccess);
        assert(removed.remaining == 2 && ble.bond_remove_calls == 1);
        assert(ble.last_bond_id == id);
        const auto route_after = runtime.state_machine().route_snapshot();
        assert(route_after.desired == route_before.desired);
        assert(route_after.active == route_before.active);
        assert(route_after.transition == route_before.transition);
        assert(route_after.generation == route_before.generation);
        assert(runtime.state_machine().authority_epoch() == authority_before);
        assert(runtime.state_machine().keyboard_state().modifiers ==
               keyboard_before.modifiers);
        assert(runtime.state_machine().mouse_state().buttons ==
               mouse_before.buttons);
    }
    {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        hide_ble(runtime, usb, ble, database, controller);
        ble.security_inhibit.inhibit(0, ble_lifecycle::kNoConnection, true);
        assert(controller.request_bond_list().kind ==
               hid_control_executor::BleBondListResultKind::kStorageFailure);
        assert(controller.request_bond_remove(id).kind ==
               hid_control_executor::BleBondRemoveResultKind::kStorageFailure);
        assert(ble.bond_list_calls == 0 && ble.bond_remove_calls == 0);
    }
    for (const auto kind : {
             hid_control_executor::BleBondRemoveResultKind::kNotFound,
             hid_control_executor::BleBondRemoveResultKind::kAmbiguous,
             hid_control_executor::BleBondRemoveResultKind::kStorageFailure,
         }) {
        hid_runtime::Runtime runtime;
        FakeBackend usb;
        FakeBleBackend ble;
        FakeBleDatabase database;
        hid_control_executor::Controller controller;
        hide_ble(runtime, usb, ble, database, controller);
        ble.bond_remove_result.kind = kind;
        assert(controller.request_bond_remove(id).kind == kind);
        assert(ble.bond_remove_calls == 1);
    }
}

}  // namespace

int main() {
    test_install_and_uninstall_are_task_owned_and_serialized();
    test_route_owner_blocks_attach_before_lifecycle_stage_a();
    test_route_owner_blocks_detach_before_lifecycle_stage_a();
    test_usb_owner_blocks_route_reservation();
    test_no_op_and_ordinary_rejection_release_guard();
    test_schedule_requires_matching_preclaimed_owner();
    test_backend_failures_release_guard();
    test_real_queue_failure_is_recovery_fault_and_releases_guard();
    test_callback_invalidation_obsoletes_action_and_releases_guard();
    test_route_zero_work_is_synchronous_and_never_uninstalls_usb();
    test_route_release_owns_guard_and_blocks_detach_before_stage_a();
    test_usb_detach_owner_blocks_route_without_duplicate_action();
    test_route_schedule_failure_terminalizes_none_and_releases_guard();
    test_unmount_preempts_route_action_and_guard_releases_once();
    test_ble_lifecycle_is_shared_serialized_and_transport_independent();
    test_ble_disable_expected_disconnect_and_retained_stack();
    test_ble_disable_preserves_active_usb_route_and_reports();
    test_ble_disable_failure_preserves_active_usb_route();
    test_usb_route_release_serializes_ble_disable_without_ble_stage_a();
    test_missing_live_database_fails_closed_before_advertising();
    test_stale_sync_cannot_validate_or_advertise();
    test_retained_cycles_validate_without_duplicate_registration();
    test_ble_busy_has_no_stage_a_and_usb_detach_does_not_change_ble();
    test_generation_owned_cccd_restore_term_and_clearing();
    test_ble_link_readiness_security_and_suspend_predicate();
    test_gatt_cache_legacy_refresh_read_persist_and_current_reconnect();
    test_gatt_cache_non_authoritative_refresh_results_and_independent_read();
    test_gatt_cache_identity_disconnect_and_subscription_fencing();
    test_gatt_cache_persistence_failures_follow_store_policy();
    test_gatt_cache_dropped_report_map_read_fails_closed();
    test_store_failure_immediately_inhibits_stale_verification();
    test_store_failure_identity_reuse_is_fenced();
    test_disable_request_defers_security_retirement_to_executor();
    test_fatal_storage_latch_preempts_disable_retirement();
    test_fatal_storage_latch_preempts_disconnect_retirement();
    test_reset_sync_normal_handoff_reaches_advertising();
    test_sync_fallback_wakes_after_executor_final_queue_drain();
    test_generic_overflow_wakes_after_executor_final_queue_drain();
    test_adopted_peer_overflow_wakes_after_final_queue_drain();
    test_fallback_wake_retention_active_and_coalesced_priority();
    test_dropped_future_sync_cannot_leave_reset_enabling();
    test_dropped_future_lifecycle_timeout_cannot_leave_reset_enabling();
    test_stale_sync_does_not_set_handoff_failure();
    test_repeated_reset_publication_failure_is_bounded();
    test_reset_sync_handoff_wrap_failure_remains_visible();
    test_dropped_connect_terminates_exact_orphan_and_ignores_disconnect();
    test_dropped_connect_termination_failure_stays_fail_closed();
    test_overflow_current_then_stale_preserves_cccd_fail_closed();
    test_overflow_stale_then_current_security_fails_closed();
    test_two_current_overflows_preserve_suspend_fail_closed();
    test_stale_overflow_does_not_poison_reused_handle_authority();
    test_overflow_consumer_cas_preserves_racing_current_producer();
    test_dropped_store_full_uses_generic_fault_not_global_fatal();
    test_overflow_generation_zero_wrap_remains_fail_closed();
    test_fatal_storage_overflow_survives_disable_generation_advance();
    test_fatal_storage_overflow_survives_disconnect();
    test_fatal_storage_without_connection_preempts_enable_and_is_idempotent();
    test_startup_store_recovery_failure_never_exposes_ble();
    test_store_full_does_not_trigger_global_fatal_reconciliation();
    test_store_full_retirement_does_not_poison_future_peer();
    test_internal_ble_notification_adapter_and_result_model();
    test_internal_ble_route_activation_and_exact_payloads();
    test_ble_route_none_usb_and_no_dual_delivery();
    test_ble_work_token_fences_every_authority_field();
    test_canceled_ble_ticket_waits_for_exact_action_acknowledgment();
    test_usb_mount_retires_ble_authority_without_forgetting_held_state();
    test_ble_readiness_loss_retires_route_without_auto_restore();
    test_callback_readiness_loss_preempts_earlier_queued_report();
    test_security_and_storage_loss_preempt_ble_work();
    test_overflow_and_store_full_retire_only_current_ble_route();
    test_ble_backend_failure_retires_and_never_replays_mouse();
    test_ble_not_ready_and_stale_results_terminalize_runtime_ticket();
    test_ble_disconnect_reconnect_kills_old_work_and_route();
    test_u74c_normal_retirement_release_grace_and_cross_transport();
    test_u74c_usb_none_ble_and_direct_switch_rejection();
    test_u74c_release_rejection_and_disconnect_failure_are_bounded();
    test_u74c_grace_queue_full_has_sticky_progress();
    test_u74c_dropped_exact_disconnect_still_completes_route();
    test_u74c_spontaneous_disconnect_and_stale_grace_are_fenced();
    test_u74c_disconnect_before_first_release_wake_releases_owner();
    test_u74c_disconnect_expiry_race_and_release_action_reuse();
    test_u74c_release_epoch_wrap_has_exact_zero_owner();
    test_u74c_lease_overflow_and_disable_retirement_paths();
    test_u74c_ble_completion_preserves_independent_usb_uncertainty();
    test_pairing_input_response_and_initiation();
    test_public_pairing_rpc_mailbox_status_and_races();
    test_security_event_ordering_and_existing_bond();
    test_pairing_completion_waits_for_post_persistence_event();
    test_missing_our_sec_is_fatal_storage_failure();
    test_missing_peer_sec_is_fatal_storage_failure();
    test_persisted_bond_reread_mismatch_is_fatal_storage_failure();
    test_low_level_storage_error_is_global_even_with_stale_identity();
    test_store_full_remains_recoverable_and_readvertises();
    test_timeout_repeat_store_and_disconnect_results();
    test_store_full_disconnect_failure_terminalizes_recovery();
    test_pairing_timeout_disconnect_failure_terminalizes_recovery();
    test_smp_and_repeat_pairing_disconnect_failures_terminalize_recovery();
    test_security_teardown_already_disconnected_reconciles_exact_peer();
    test_security_teardown_normal_and_failed_disconnect_results();
    test_already_disconnected_identity_late_event_and_handle_reuse_fences();
    test_already_disconnected_completes_releasing_ble_route();
    test_queue_burst_overflow_and_id_wrap_fail_closed();
    test_policy_persistence_disconnect_disable_and_bounded_burst();
    test_bond_administration_serialization_and_safety_policy();
}
