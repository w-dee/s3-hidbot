#include "hid_control_executor/hid_control_executor.hpp"

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

namespace hid_control_executor {
namespace {

#ifndef HID_CONTROL_EXECUTOR_NATIVE_TEST
constexpr std::size_t kActionQueueDepth = 2;
constexpr std::uint32_t kLifecycleTaskStackBytes = 4096;
constexpr std::size_t kLifecycleTaskStackDepth =
    kLifecycleTaskStackBytes / sizeof(StackType_t);
constexpr UBaseType_t kLifecycleTaskPriority = tskIDLE_PRIORITY + 3;

StaticQueue_t s_queue_storage;
std::uint8_t s_queue_bytes[kActionQueueDepth * sizeof(Controller::Action)]{};
StaticTask_t s_task_storage;
StackType_t s_task_stack[kLifecycleTaskStackDepth]{};
QueueHandle_t s_action_queue = nullptr;
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
    if (s_action_queue == nullptr ||
        xTaskCreateStatic(task_entry, "hid_control", kLifecycleTaskStackDepth,
                          this, kLifecycleTaskPriority, s_task_stack, &s_task_storage) == nullptr) {
        return false;
    }
#endif
    initialized_ = true;
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
    const ble_lifecycle::TransitionOutcome outcome = ble_state_.begin_disable();
    if (outcome.action_result == ble_lifecycle::TransitionResult::kAccepted) {
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

bool Controller::signal_ble_event(BleEvent event) {
    const Action item{.kind = ActionKind::kBleEvent, .ble_event = event};
    if (enqueue(item)) {
        return true;
    }
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
    if (native_count_ == 2) {
        return false;
    }
    native_queue_[(native_head_ + native_count_) % 2] = item;
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
    hid_runtime::StateMachine &state = runtime_->state_machine();
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

void Controller::process_ble_event(BleEvent event) {
    if (ble_backend_ == nullptr) {
        return;
    }
    if (ble_event_overflow_.exchange(false, std::memory_order_acq_rel)) {
        fail_ble(ble_state_.generation(), ble_lifecycle::Operation::kRuntime, -2,
                 active_operation_.load(std::memory_order_acquire));
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
            (void)ble_state_.observe_connect(event.generation,
                                             event.connection_handle);
            return;
        case BleEventKind::kDisconnect: {
            const bool expected = active_operation_.load(std::memory_order_acquire) ==
                                  ControlOperation::kBleDisable;
            if (!ble_state_.observe_disconnect(event.generation,
                                               event.connection_handle, expected)) {
                return;
            }
            if (ble_database_ != nullptr) {
                ble_database_->clear_peer_state();
            }
            if (expected) {
                ble_state_.complete_disable(event.generation);
                release_operation(ControlOperation::kBleDisable);
                return;
            }
            const auto generation = ble_state_.generation();
            ble_backend_->set_generation(generation);
            const std::int32_t result = ble_backend_->start_advertising();
            if (result == 0) {
                ble_state_.complete_advertising(generation);
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
    }
}

#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
bool Controller::process_one_for_test() {
    if (native_count_ == 0) {
        return false;
    }
    const Action action = native_queue_[native_head_];
    native_head_ = static_cast<std::uint8_t>((native_head_ + 1) % 2);
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
