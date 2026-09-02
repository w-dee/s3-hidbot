#include "hid_control_executor/hid_control_executor.hpp"

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#include <cstring>

#include "secure_memory/secure_memory.hpp"

namespace hid_control_executor {
namespace {

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
constexpr std::size_t kActionQueueDepth = 8;
constexpr std::uint32_t kLifecycleTaskStackBytes = 4096;
constexpr std::size_t kLifecycleTaskStackDepth =
    kLifecycleTaskStackBytes / sizeof(StackType_t);
constexpr UBaseType_t kLifecycleTaskPriority = tskIDLE_PRIORITY + 3;

StaticQueue_t s_queue_storage;
std::uint8_t s_queue_bytes[kActionQueueDepth * sizeof(Controller::Action)]{};
StaticTask_t s_task_storage;
StackType_t s_task_stack[kLifecycleTaskStackDepth]{};
QueueHandle_t s_action_queue = nullptr;
StaticSemaphore_t s_pairing_rpc_completion_storage;
SemaphoreHandle_t s_pairing_rpc_completion = nullptr;
#endif

}  // namespace

bool Controller::initialize(hid_runtime::Runtime *runtime, Backend *backend,
                            BleBackend *ble_backend, BleDatabase *ble_database) {
    if (initialized_) {
        return true;
    }
    if (runtime == nullptr || backend == nullptr) {
        return false;
    }
    runtime_ = runtime;
    backend_ = backend;
    ble_backend_ = ble_backend;
    ble_database_ = ble_database;
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    s_action_queue = xQueueCreateStatic(kActionQueueDepth, sizeof(Action), s_queue_bytes,
                                        &s_queue_storage);
    s_pairing_rpc_completion =
        xSemaphoreCreateBinaryStatic(&s_pairing_rpc_completion_storage);
    if (s_action_queue == nullptr || s_pairing_rpc_completion == nullptr ||
        xTaskCreateStatic(task_entry, "hid_control", kLifecycleTaskStackDepth,
                          this, kLifecycleTaskPriority, s_task_stack, &s_task_storage) == nullptr) {
        return false;
    }
#endif
    initialized_ = true;
    if (ble_backend_ != nullptr) {
        ble_backend_->record_heap_checkpoint(BleBackend::HeapCheckpoint::kColdBoot);
    }
    return true;
}

BleCommandOutcome Controller::request_ble_enable() {
    constexpr ControlOperation operation = ControlOperation::kBleEnable;
    if (!initialized_ || ble_backend_ == nullptr || !claim_operation(operation)) {
        return {};
    }
    const ble_lifecycle::TransitionOutcome outcome = ble_state_.begin_enable();
    if (outcome.action_result == ble_lifecycle::TransitionResult::kAccepted) {
        const Action item{.kind = ActionKind::kBleEnable,
                          .operation = operation};
        if (!enqueue(item)) {
            ble_state_.complete_fault(outcome.snapshot.generation,
                                      ble_lifecycle::Operation::kEnable, -1);
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    return {.action_result = outcome.action_result,
            .snapshot_valid = outcome.snapshot_valid,
            .snapshot = outcome.snapshot};
}

BleCommandOutcome Controller::request_ble_disable() {
    constexpr ControlOperation operation = ControlOperation::kBleDisable;
    if (!initialized_ || ble_backend_ == nullptr || !claim_operation(operation)) {
        return {};
    }
    const auto before = ble_state_.snapshot();
    const ble_lifecycle::TransitionOutcome outcome = ble_state_.begin_disable();
    if (outcome.action_result == ble_lifecycle::TransitionResult::kAccepted) {
        wipe_pairing_mailbox();
        pairing_deadline_us_ = 0;
        pairing_state_.disable();
        pairing_complete_seen_ = false;
        ble_backend_->cancel_pairing_timeout();
        if (before.connected) {
            ble_backend_->retire_security(before.generation,
                                          ble_state_.connection_handle());
        }
        ble_backend_->set_generation(outcome.snapshot.generation);
        const Action item{.kind = ActionKind::kBleDisable,
                          .operation = operation};
        if (!enqueue(item)) {
            ble_state_.complete_fault(outcome.snapshot.generation,
                                      ble_lifecycle::Operation::kDisable, -1);
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    return {.action_result = outcome.action_result,
            .snapshot_valid = outcome.snapshot_valid,
            .snapshot = outcome.snapshot};
}

ble_lifecycle::Snapshot Controller::ble_snapshot() const {
    return ble_state_.snapshot();
}

ble_pairing::Snapshot Controller::pairing_snapshot() const {
    return pairing_state_.snapshot();
}

PairingStatusSnapshot Controller::request_pairing_status() {
    if (!initialized_ || ble_backend_ == nullptr ||
        pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
        return {};
    }
    std::uint32_t token = next_pairing_rpc_token_++;
    if (token == 0) {
        token = next_pairing_rpc_token_++;
    }
    pairing_rpc_pending_.store(token, std::memory_order_release);
    const Action item{.kind = ActionKind::kPairingStatus,
                      .mailbox_token = token};
    if (!enqueue(item)) {
        pairing_rpc_pending_.store(0, std::memory_order_release);
        return {};
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return pairing_rpc_status_;
}

ble_pairing::RespondResult Controller::request_pairing_response(
    std::uint32_t pairing_id,
    const std::array<char, 6> &six_digit_secret) {
    if (!initialized_ || ble_backend_ == nullptr ||
        pairing_rpc_pending_.load(std::memory_order_acquire) != 0) {
        return ble_pairing::RespondResult::kNotPending;
    }
    const auto pending = pairing_state_.snapshot();
    std::uint32_t token = next_pairing_rpc_token_++;
    if (token == 0) {
        token = next_pairing_rpc_token_++;
    }
    pairing_mailbox_.generation = pending.generation;
    pairing_mailbox_.connection_handle = pending.connection_handle;
    pairing_mailbox_.pairing_id = pairing_id;
    pairing_mailbox_.secret = six_digit_secret;
    pairing_mailbox_.token = token;
    pairing_mailbox_.occupied = true;
    pairing_rpc_result_ = ble_pairing::RespondResult::kNotPending;
    pairing_rpc_pending_.store(token, std::memory_order_release);
    const Action item{.kind = ActionKind::kPairingRespond,
                      .mailbox_token = token};
    if (!enqueue(item)) {
        wipe_pairing_mailbox();
        pairing_rpc_pending_.store(0, std::memory_order_release);
        return ble_pairing::RespondResult::kNotPending;
    }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    while (pairing_rpc_pending_.load(std::memory_order_acquire) == token &&
           process_one_for_test()) {
    }
#else
    (void)xSemaphoreTake(s_pairing_rpc_completion, portMAX_DELAY);
#endif
    return pairing_rpc_result_;
}

bool Controller::signal_ble_event(BleEvent event) {
    const Action item{.kind = ActionKind::kBleEvent, .ble_event = event};
    if (enqueue(item)) {
        return true;
    }
    overflow_generation_.store(event.generation, std::memory_order_relaxed);
    overflow_connection_.store(event.connection_handle,
                               std::memory_order_relaxed);
    ble_event_overflow_.store(true, std::memory_order_release);
    return false;
}

ControlOperation Controller::operation_for(usb_lifecycle::ExecutorAction action) {
    return action == usb_lifecycle::ExecutorAction::kInstall
               ? ControlOperation::kUsbAttach
               : ControlOperation::kUsbDetach;
}

bool Controller::claim_operation(ControlOperation operation) {
    ControlOperation expected = ControlOperation::kNone;
    return active_operation_.compare_exchange_strong(expected, operation,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

void Controller::release_operation(ControlOperation operation) {
    ControlOperation expected = operation;
    (void)active_operation_.compare_exchange_strong(expected, ControlOperation::kNone,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire);
}

CommandOutcome Controller::request_attach() {
    constexpr ControlOperation operation = ControlOperation::kUsbAttach;
    if (!initialized_ || !claim_operation(operation)) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_attach(*this);
    if (outcome.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        // No asynchronous action exists for no-op, ordinary rejection, or a
        // real post-Stage-A scheduling failure.
        release_operation(operation);
    }
    return CommandOutcome{
        .action_result = outcome.action_result,
        .snapshot_valid = outcome.snapshot_valid,
        .snapshot = ExposureSnapshot{
            .lifecycle = outcome.lifecycle,
            .runtime = outcome.runtime,
        },
    };
}

CommandOutcome Controller::request_detach() {
    constexpr ControlOperation operation = ControlOperation::kUsbDetach;
    if (!initialized_ || !claim_operation(operation)) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_detach(*this);
    if (outcome.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        // Accepted work retains ownership until process() terminalizes it.
        release_operation(operation);
    }
    return CommandOutcome{
        .action_result = outcome.action_result,
        .snapshot_valid = outcome.snapshot_valid,
        .snapshot = ExposureSnapshot{
            .lifecycle = outcome.lifecycle,
            .runtime = outcome.runtime,
        },
    };
}

ExposureSnapshot Controller::snapshot() const {
    if (runtime_ == nullptr) {
        return {};
    }
    return ExposureSnapshot{
        .lifecycle = runtime_->state_machine().usb_lifecycle_snapshot(),
        .runtime = runtime_->state_machine().status(),
    };
}

hid_runtime::RouteStatusSnapshot Controller::route_snapshot() const {
    return runtime_ == nullptr ? hid_runtime::RouteStatusSnapshot{}
                               : runtime_->state_machine().route_status_snapshot();
}

RouteCommandOutcome Controller::request_route(hid_route::OutputRoute desired) {
    constexpr ControlOperation operation = ControlOperation::kRouteChange;
    if (!initialized_ || desired == hid_route::OutputRoute::kBle ||
        !claim_operation(operation)) {
        return {};
    }
    hid_runtime::RouteTransitionOutcome outcome =
        desired == hid_route::OutputRoute::kUsb
            ? runtime_->state_machine().request_route_usb()
            : runtime_->state_machine().request_route_none();
    if (outcome.action_result == hid_runtime::RouteTransitionResult::kAccepted &&
        outcome.async_required) {
        const Action item{
            .kind = ActionKind::kRouteRelease,
            .lifecycle = runtime_->state_machine().usb_lifecycle_snapshot(),
            .route = outcome.snapshot.route,
            .operation = operation,
        };
        if (!enqueue(item)) {
            runtime_->state_machine().terminalize_route_release_schedule_failure(
                outcome.snapshot.route);
            release_operation(operation);
            return {};
        }
    } else {
        release_operation(operation);
    }
    return RouteCommandOutcome{.action_result = outcome.action_result,
                               .snapshot_valid = outcome.snapshot_valid,
                               .snapshot = outcome.snapshot};
}

bool Controller::enqueue(Action item) {
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (fail_next_enqueue_) {
        fail_next_enqueue_ = false;
        return false;
    }
    if (native_count_ == 8) {
        return false;
    }
    native_queue_[(native_head_ + native_count_) % 8] = item;
    ++native_count_;
    return true;
#else
    return xQueueSend(s_action_queue, &item, 0) == pdPASS;
#endif
}

bool Controller::schedule(usb_lifecycle::ExecutorAction action,
                          usb_lifecycle::Snapshot snapshot) {
    const ControlOperation operation = operation_for(action);
    // The request entry point must own the cross-domain guard before USB
    // lifecycle Stage-A. Scheduling only verifies inherited ownership.
    if (!initialized_ ||
        active_operation_.load(std::memory_order_acquire) != operation) {
        return false;
    }
    const Action item{.kind = action == usb_lifecycle::ExecutorAction::kInstall
                                  ? ActionKind::kUsbInstall
                                  : ActionKind::kUsbDetach,
                      .lifecycle = snapshot,
                      .route = runtime_->state_machine().route_snapshot(),
                      .operation = operation};
    return enqueue(item);
}

void Controller::process(Action action) {
    if (runtime_ == nullptr || backend_ == nullptr) {
        release_operation(action.operation);
        return;
    }
    if (consume_ble_overflow() && action.kind == ActionKind::kBleEvent) {
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    if (action.kind == ActionKind::kPairingStatus) {
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            action.mailbox_token) {
            return;
        }
        reconcile_pairing_deadline();
        pairing_rpc_status_ = current_pairing_status();
        complete_pairing_rpc(action.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kPairingRespond) {
        if (pairing_rpc_pending_.load(std::memory_order_acquire) !=
            action.mailbox_token) {
            return;
        }
        std::array<char, 6> secret{};
        const bool mailbox_current = pairing_mailbox_.occupied &&
                                     pairing_mailbox_.token == action.mailbox_token;
        const auto generation = pairing_mailbox_.generation;
        const auto connection = pairing_mailbox_.connection_handle;
        const auto pairing_id = pairing_mailbox_.pairing_id;
        if (mailbox_current) {
            secret = pairing_mailbox_.secret;
        }
        wipe_pairing_mailbox();
        reconcile_pairing_deadline();
        pairing_rpc_result_ = mailbox_current
            ? respond_to_pairing(generation, connection, pairing_id, secret)
            : ble_pairing::RespondResult::kNotPending;
        secure_memory::zero(secret.data(), secret.size());
        complete_pairing_rpc(action.mailbox_token);
        return;
    }
    if (action.kind == ActionKind::kBleEvent) {
        process_ble_event(action.ble_event);
        return;
    }
    if (action.kind == ActionKind::kBleEnable) {
        const auto current = ble_state_.snapshot();
        if (active_operation_.load(std::memory_order_acquire) != action.operation ||
            current.desired != ble_lifecycle::DesiredExposure::kExposed ||
            current.observed != ble_lifecycle::ObservedState::kEnabling) {
            release_operation(action.operation);
            return;
        }
        ble_backend_->set_generation(current.generation);
        if (!current.stack_ready) {
            ble_backend_->record_heap_checkpoint(
                BleBackend::HeapCheckpoint::kBeforeFirstEnable);
            const std::int32_t result = ble_backend_->initialize(
                this, ble_database_, current.generation);
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kEnable,
                         result, action.operation);
            }
            return;
        }
        const std::int32_t result = ble_backend_->start_advertising();
        if (result == 0) {
            ble_state_.complete_advertising(current.generation);
            ble_backend_->record_heap_checkpoint(
                BleBackend::HeapCheckpoint::kAdvertising);
            release_operation(action.operation);
        } else {
            fail_ble(current.generation, ble_lifecycle::Operation::kEnable, result,
                     action.operation);
        }
        return;
    }
    if (action.kind == ActionKind::kBleDisable) {
        const auto current = ble_state_.snapshot();
        if (active_operation_.load(std::memory_order_acquire) != action.operation ||
            current.desired != ble_lifecycle::DesiredExposure::kHidden ||
            current.observed != ble_lifecycle::ObservedState::kDisabling) {
            release_operation(action.operation);
            return;
        }
        if (current.advertising) {
            const std::int32_t result = ble_backend_->stop_advertising();
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kDisable,
                         result, action.operation);
                return;
            }
        }
        if (current.connected) {
            const std::int32_t result =
                ble_backend_->disconnect(ble_state_.connection_handle());
            if (result != 0) {
                fail_ble(current.generation, ble_lifecycle::Operation::kDisable,
                         result, action.operation);
            }
            return;
        }
        if (ble_database_ != nullptr) {
            ble_database_->clear_peer_state();
        }
        ble_state_.complete_disable(current.generation);
        ble_backend_->record_heap_checkpoint(
            BleBackend::HeapCheckpoint::kHiddenIdle);
        release_operation(action.operation);
        return;
    }
    if (action.kind == ActionKind::kRouteRelease) {
        const hid_route::Snapshot route = state.route_snapshot();
        const usb_lifecycle::Snapshot lifecycle = state.usb_lifecycle_snapshot();
        const bool expected_route = route.coherent && !route.invalidation_pending &&
                                    route.desired == hid_route::OutputRoute::kNone &&
                                    route.active == hid_route::OutputRoute::kUsb &&
                                    route.transition == hid_route::Transition::kReleasing &&
                                    route.generation == action.route.generation;
        const bool expected_transport =
            lifecycle.generation == action.lifecycle.generation &&
            lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed &&
            lifecycle.observed == usb_lifecycle::ObservedState::kMounted;
        if (active_operation_.load(std::memory_order_acquire) != action.operation ||
            !expected_route || !expected_transport) {
            if (expected_route) {
                state.terminalize_route_release_schedule_failure(action.route);
            }
            release_operation(action.operation);
            return;
        }
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
        const hid_runtime::LifecycleSafetyResult safety =
            state.begin_route_release_safety(action.route);
        if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
            state.mark_lifecycle_detach_uncertain(action.lifecycle.generation);
        }
#else
        (void)runtime_->run_route_release_safety(action.route);
#endif
        state.complete_route_release(action.route);
        release_operation(action.operation);
        return;
    }
    const usb_lifecycle::Snapshot current = state.usb_lifecycle_snapshot();
    const bool expected_lifecycle_state =
        action.kind == ActionKind::kUsbInstall
            ? current.desired == usb_lifecycle::DesiredExposure::kExposed &&
                  current.observed == usb_lifecycle::ObservedState::kAttaching
            : current.desired == usb_lifecycle::DesiredExposure::kHidden &&
                  current.observed == usb_lifecycle::ObservedState::kDetaching;
    if (active_operation_.load(std::memory_order_acquire) != action.operation ||
        !expected_lifecycle_state ||
        current.desired != action.lifecycle.desired ||
        current.observed != action.lifecycle.observed ||
        current.generation != action.lifecycle.generation) {
        release_operation(action.operation);
        return;
    }
    if (action.kind == ActionKind::kUsbInstall) {
        const BackendResult result = backend_->install();
        switch (result.kind) {
            case BackendResultKind::kSuccess:
                // The mount callback may already have changed observed to
                // mounted; complete_install_success intentionally preserves it.
                state.complete_usb_install_success();
                break;
            case BackendResultKind::kCleanInstallFailure:
                state.complete_usb_install_clean_failure(result.error_code);
                break;
            case BackendResultKind::kAmbiguousInstallFailure:
                state.complete_usb_install_ambiguous_failure(result.error_code);
                break;
            case BackendResultKind::kUninstallFailure:
                state.complete_usb_install_ambiguous_failure(result.error_code);
                break;
        }
        release_operation(action.operation);
        return;
    }

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    const hid_runtime::LifecycleSafetyResult safety = state.begin_lifecycle_detach_safety();
    if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
        state.mark_lifecycle_detach_uncertain(action.lifecycle.generation);
    }
#else
    (void)runtime_->run_lifecycle_detach_safety();
#endif
    state.complete_usb_detach_route_invalidation(action.route);
    state.begin_usb_uninstall();
    const BackendResult result = backend_->uninstall();
    if (result.kind == BackendResultKind::kSuccess) {
        state.on_driver_uninstalled();
        state.complete_usb_uninstall_success();
    } else {
        state.complete_usb_uninstall_failure(result.error_code);
    }
    release_operation(action.operation);
}

void Controller::fail_ble(ble_lifecycle::Generation generation,
                          ble_lifecycle::Operation operation, std::int32_t code,
                          ControlOperation owner) {
    ble_state_.complete_fault(generation, operation, code);
    if (ble_database_ != nullptr) {
        ble_database_->clear_peer_state();
    }
    release_operation(owner);
}

bool Controller::consume_ble_overflow() {
    if (!ble_event_overflow_.exchange(false, std::memory_order_acq_rel) ||
        ble_backend_ == nullptr) {
        return false;
    }
    const auto current = ble_state_.snapshot();
    const auto generation =
        overflow_generation_.load(std::memory_order_acquire);
    const auto connection =
        overflow_connection_.load(std::memory_order_acquire);
    if (generation != current.generation ||
        (connection != ble_lifecycle::kNoConnection && current.connected &&
         connection != ble_state_.connection_handle())) {
        return false;
    }
    pairing_state_.fail_closed(current.generation,
                               ble_state_.connection_handle(),
                               ble_pairing::LastResult::kQueueOverflow);
    pairing_deadline_us_ = 0;
    wipe_pairing_mailbox();
    ble_backend_->cancel_pairing_timeout();
    ble_backend_->mark_security_unhealthy(current.generation);
    if (current.connected) {
        (void)ble_backend_->disconnect(ble_state_.connection_handle());
    }
    const auto active = active_operation_.load(std::memory_order_acquire);
    const auto ble_owner =
        active == ControlOperation::kBleEnable ||
                active == ControlOperation::kBleDisable
            ? active
            : ControlOperation::kNone;
    fail_ble(current.generation, ble_lifecycle::Operation::kRuntime, -2,
             ble_owner);
    return true;
}

void Controller::terminate_security_connection(ble_pairing::LastResult result,
                                               bool fatal) {
    const auto current = ble_state_.snapshot();
    if (!current.connected || ble_backend_ == nullptr) {
        return;
    }
    const auto handle = ble_state_.connection_handle();
    pairing_deadline_us_ = 0;
    wipe_pairing_mailbox();
    (void)pairing_state_.complete(current.generation, handle, result);
    ble_backend_->cancel_pairing_timeout();
    ble_backend_->retire_security(current.generation, handle);
    (void)ble_backend_->disconnect(handle);
    if (fatal) {
        ble_backend_->mark_security_unhealthy(current.generation);
        fail_ble(current.generation, ble_lifecycle::Operation::kRuntime, -3,
                 ControlOperation::kNone);
    }
}

void Controller::reconcile_security(std::uint16_t connection_handle,
                                    bool pairing_complete_seen) {
    const auto current = ble_state_.snapshot();
    if (!current.connected || current.generation != ble_state_.generation() ||
        connection_handle != ble_state_.connection_handle()) {
        return;
    }
    pairing_complete_seen_ = pairing_complete_seen_ || pairing_complete_seen;
    ble_backend_->refresh_security(connection_handle);
    if (ble_backend_->security_ready_for_hid(current.generation,
                                             connection_handle)) {
        pairing_state_.complete(current.generation, connection_handle,
                                ble_pairing::LastResult::kSucceeded);
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        ble_backend_->cancel_pairing_timeout();
        return;
    }
    const auto security = ble_backend_->security_snapshot();
    if (pairing_complete_seen_ && security.coherent && security.connected &&
        security.encrypted && security.identity_resolved &&
        security.store_healthy) {
        if (!security.authenticated ||
            security.key_size != ble_security::kRequiredKeySize) {
            terminate_security_connection(
                ble_pairing::LastResult::kSecurityPolicy, false);
        } else if (!security.project_verified_bond_persisted) {
            terminate_security_connection(ble_pairing::LastResult::kStorage,
                                          true);
        }
    }
}

void Controller::reconcile_pairing_deadline() {
    if (ble_backend_ == nullptr || pairing_deadline_us_ == 0) {
        return;
    }
    const auto pending = pairing_state_.snapshot();
    if (pending.live_state != ble_pairing::LiveState::kWaitingInput ||
        !pending.pairing_active) {
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        return;
    }
    if (ble_backend_->monotonic_time_us() < pairing_deadline_us_) {
        return;
    }
    if (pairing_state_.timeout(pending.generation, pending.connection_handle,
                               pending.pairing_id)) {
        pairing_deadline_us_ = 0;
        wipe_pairing_mailbox();
        terminate_security_connection(ble_pairing::LastResult::kTimeout, false);
    }
}

PairingStatusSnapshot Controller::current_pairing_status() const {
    PairingStatusSnapshot result{};
    result.available = true;
    result.pairing = pairing_state_.snapshot();
    const auto lifecycle = ble_state_.snapshot();
    result.generation = lifecycle.generation;
    result.connected = lifecycle.connected;
    if (ble_backend_ != nullptr) {
        const auto security = ble_backend_->security_snapshot();
        if (security.coherent && security.generation == lifecycle.generation &&
            security.connected == lifecycle.connected) {
            result.security = security;
        }
        if (result.pairing.live_state == ble_pairing::LiveState::kWaitingInput &&
            result.pairing.pairing_active && pairing_deadline_us_ != 0) {
            const std::uint64_t now = ble_backend_->monotonic_time_us();
            if (now < pairing_deadline_us_) {
                const std::uint64_t remaining = pairing_deadline_us_ - now;
                result.remaining_ms = static_cast<std::uint32_t>(
                    (remaining + 999U) / 1000U);
            }
        }
    }
    return result;
}

void Controller::wipe_pairing_mailbox() {
    secure_memory::zero(&pairing_mailbox_, sizeof(pairing_mailbox_));
}

void Controller::complete_pairing_rpc(std::uint32_t token) {
    std::uint32_t expected = token;
    if (!pairing_rpc_pending_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
    (void)xSemaphoreGive(s_pairing_rpc_completion);
#endif
}

ble_pairing::RespondResult Controller::respond_to_pairing(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint32_t pairing_id,
    const std::array<char, 6> &six_digit_secret) {
    if (!initialized_ || ble_backend_ == nullptr) {
        return ble_pairing::RespondResult::kNotPending;
    }
    auto result = pairing_state_.validate_response(
        generation, connection_handle, pairing_id);
    if (result != ble_pairing::RespondResult::kAccepted) {
        return result;
    }
    std::uint32_t value = 0;
    for (const char digit : six_digit_secret) {
        if (digit < '0' || digit > '9') {
            return ble_pairing::RespondResult::kInvalidSecret;
        }
        value = value * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    const std::int32_t injection_result =
        ble_backend_->inject_passkey(connection_handle, value);
    secure_memory::zero(&value, sizeof(value));
    if (injection_result != 0) {
        terminate_security_connection(ble_pairing::LastResult::kSmpFailed,
                                      false);
        return ble_pairing::RespondResult::kInjectionFailed;
    }
    pairing_state_.consume_response(generation, connection_handle, pairing_id);
    pairing_deadline_us_ = 0;
    ble_backend_->cancel_pairing_timeout();
    return ble_pairing::RespondResult::kAccepted;
}

void Controller::process_ble_event(BleEvent event) {
    if (ble_backend_ == nullptr) {
        return;
    }
    switch (event.kind) {
        case BleEventKind::kSync: {
            if (!ble_state_.complete_sync(event.generation)) {
                return;
            }
            const std::int32_t result = ble_backend_->start_advertising();
            if (result == 0) {
                ble_state_.complete_advertising(event.generation);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kAdvertising);
                const ControlOperation owner =
                    active_operation_.load(std::memory_order_acquire);
                if (owner == ControlOperation::kBleEnable) {
                    release_operation(owner);
                }
            } else {
                fail_ble(event.generation, ble_lifecycle::Operation::kEnable,
                         result, ControlOperation::kBleEnable);
            }
            return;
        }
        case BleEventKind::kConnect:
            if (ble_state_.observe_connect(event.generation,
                                           event.connection_handle)) {
                pairing_complete_seen_ = false;
                pairing_deadline_us_ = 0;
                wipe_pairing_mailbox();
                pairing_state_.begin_connection(event.generation,
                                                event.connection_handle);
                ble_backend_->begin_security(event.generation,
                                             event.connection_handle);
                (void)ble_backend_->configure_connection(event.connection_handle);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kConnected);
                const std::int32_t result =
                    ble_backend_->initiate_security(event.connection_handle);
                if (result != 0) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSmpFailed, false);
                }
            }
            return;
        case BleEventKind::kDisconnect: {
            const bool expected = active_operation_.load(std::memory_order_acquire) ==
                                  ControlOperation::kBleDisable;
            if (!ble_state_.observe_disconnect(event.generation,
                                               event.connection_handle, expected)) {
                return;
            }
            ble_backend_->cancel_pairing_timeout();
            pairing_deadline_us_ = 0;
            wipe_pairing_mailbox();
            pairing_complete_seen_ = false;
            (void)pairing_state_.disconnect(event.generation,
                                            event.connection_handle);
            ble_backend_->retire_security(event.generation,
                                          event.connection_handle);
            if (ble_database_ != nullptr) {
                ble_database_->clear_peer_state();
            }
            if (expected) {
                ble_state_.complete_disable(event.generation);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kHiddenIdle);
                release_operation(ControlOperation::kBleDisable);
                return;
            }
            const auto generation = ble_state_.generation();
            ble_backend_->set_generation(generation);
            const std::int32_t result = ble_backend_->start_advertising();
            if (result == 0) {
                ble_state_.complete_advertising(generation);
                ble_backend_->record_heap_checkpoint(
                    BleBackend::HeapCheckpoint::kReadvertising);
            } else {
                fail_ble(generation, ble_lifecycle::Operation::kRuntime, result,
                         ControlOperation::kNone);
            }
            return;
        }
        case BleEventKind::kAdvertisingComplete:
            // A completion for an obsolete generation is ignored. An active
            // exposed incarnation is restarted by the serialized owner.
            if (event.generation == ble_state_.generation() &&
                ble_state_.snapshot().desired ==
                    ble_lifecycle::DesiredExposure::kExposed &&
                !ble_state_.snapshot().connected) {
                const std::int32_t result = ble_backend_->start_advertising();
                if (result != 0) {
                    fail_ble(event.generation, ble_lifecycle::Operation::kRuntime,
                             result, ControlOperation::kNone);
                }
            }
            return;
        case BleEventKind::kReset: {
            const auto before_reset = ble_state_.snapshot();
            pairing_state_.reset();
            pairing_deadline_us_ = 0;
            wipe_pairing_mailbox();
            ble_backend_->cancel_pairing_timeout();
            ble_backend_->mark_security_unhealthy(event.generation);
            if (before_reset.connected) {
                ble_backend_->retire_security(event.generation,
                                              ble_state_.connection_handle());
            }
            pairing_complete_seen_ = false;
            if (!ble_state_.begin_reset_recovery(event.generation, event.status)) {
                if (ble_database_ != nullptr) {
                    ble_database_->clear_peer_state();
                }
                release_operation(active_operation_.load(std::memory_order_acquire));
                return;
            }
            const auto generation = ble_state_.generation();
            ble_backend_->set_generation(generation);
            return;
        }
        case BleEventKind::kTimeout:
            fail_ble(event.generation, ble_lifecycle::Operation::kRuntime,
                     event.status, active_operation_.load(std::memory_order_acquire));
            return;
        case BleEventKind::kPasskeyAction: {
            if (event.status == 1) {
                const auto decision = pairing_state_.begin_passkey_input(
                    event.generation, event.connection_handle);
                if (decision == ble_pairing::PasskeyActionResult::kStarted) {
                    pairing_deadline_us_ = ble_backend_->monotonic_time_us() +
                        static_cast<std::uint64_t>(ble_pairing::kInputTimeoutMs) * 1000U;
                    ble_backend_->arm_pairing_timeout(
                        event.generation, event.connection_handle,
                        pairing_state_.snapshot().pairing_id);
                } else if (decision ==
                           ble_pairing::PasskeyActionResult::kIdExhausted) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSecurityPolicy, true);
                }
            } else {
                const auto decision = pairing_state_.reject_unsupported_action(
                    event.generation, event.connection_handle);
                if (decision ==
                    ble_pairing::PasskeyActionResult::kUnsupported) {
                    terminate_security_connection(
                        ble_pairing::LastResult::kSecurityPolicy, false);
                }
            }
            return;
        }
        case BleEventKind::kEncryptionChange:
            if (event.generation != ble_state_.generation() ||
                event.connection_handle != ble_state_.connection_handle()) {
                return;
            }
            if (event.status != 0) {
                terminate_security_connection(
                    ble_pairing::LastResult::kSmpFailed, false);
                return;
            }
            reconcile_security(event.connection_handle, false);
            return;
        case BleEventKind::kPairingComplete:
            if (event.generation != ble_state_.generation() ||
                event.connection_handle != ble_state_.connection_handle()) {
                return;
            }
            if (event.status != 0) {
                terminate_security_connection(
                    ble_pairing::LastResult::kSmpFailed, false);
                return;
            }
            reconcile_security(event.connection_handle, true);
            return;
        case BleEventKind::kIdentityResolved:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                ble_backend_->refresh_security(event.connection_handle, true);
                reconcile_security(event.connection_handle, false);
            }
            return;
        case BleEventKind::kRepeatPairing:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                terminate_security_connection(
                    ble_pairing::LastResult::kRepeatPairing, false);
            }
            return;
        case BleEventKind::kPairingTimeout:
            if (pairing_state_.timeout(event.generation,
                                       event.connection_handle,
                                       event.pairing_id)) {
                pairing_deadline_us_ = 0;
                wipe_pairing_mailbox();
                terminate_security_connection(
                    ble_pairing::LastResult::kTimeout, false);
            }
            return;
        case BleEventKind::kStoreFull:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                terminate_security_connection(
                    ble_pairing::LastResult::kStoreFull, false);
            }
            return;
        case BleEventKind::kStorageFailure:
            if (event.generation == ble_state_.generation() &&
                event.connection_handle == ble_state_.connection_handle()) {
                terminate_security_connection(
                    ble_pairing::LastResult::kStorage, true);
            }
            return;
    }
}

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
bool Controller::process_one_for_test() {
    if (native_count_ == 0) {
        return false;
    }
    const Action action = native_queue_[native_head_];
    native_head_ = static_cast<std::uint8_t>((native_head_ + 1) % 8);
    --native_count_;
    process(action);
    return true;
}

ControlOperation Controller::active_operation_for_test() const {
    return active_operation_.load(std::memory_order_acquire);
}

bool Controller::reserve_operation_for_test(ControlOperation operation) {
    return operation != ControlOperation::kNone && claim_operation(operation);
}

void Controller::release_operation_for_test(ControlOperation operation) {
    release_operation(operation);
}

void Controller::fail_next_enqueue_for_test() { fail_next_enqueue_ = true; }

void Controller::set_next_pairing_id_for_test(std::uint32_t value) {
    pairing_state_.set_next_pairing_id_for_test(value);
}

bool Controller::pairing_mailbox_zero_for_test() const {
    const auto *bytes =
        reinterpret_cast<const unsigned char *>(&pairing_mailbox_);
    for (std::size_t index = 0; index < sizeof(pairing_mailbox_); ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}
#else
void Controller::task_entry(void *context) {
    static_cast<Controller *>(context)->task_loop();
}

void Controller::task_loop() {
    while (true) {
        Action action{};
        if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) == pdPASS) {
            process(action);
        }
    }
}
#endif

}  // namespace hid_control_executor
