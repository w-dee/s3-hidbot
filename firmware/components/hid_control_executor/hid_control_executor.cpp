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

bool Controller::initialize(hid_runtime::Runtime *runtime, Backend *backend) {
    if (initialized_) {
        return true;
    }
    if (runtime == nullptr || backend == nullptr) {
        return false;
    }
    runtime_ = runtime;
    backend_ = backend;
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
    if (!initialized_) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_attach(*this);
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
    if (!initialized_) {
        return {};
    }
    const hid_runtime::UsbTransitionOutcome outcome =
        runtime_->state_machine().request_usb_detach(*this);
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

bool Controller::schedule(usb_lifecycle::ExecutorAction action,
                          usb_lifecycle::Snapshot snapshot) {
    const ControlOperation operation = operation_for(action);
    if (!initialized_ || !claim_operation(operation)) {
        return false;
    }
    const Action item{.kind = action,
                      .snapshot = snapshot,
                      .route = runtime_->state_machine().route_snapshot(),
                      .operation = operation};
#ifdef HID_CONTROL_EXECUTOR_NATIVE_TEST
    if (native_count_ == 2) {
        release_operation(operation);
        return false;
    }
    native_queue_[(native_head_ + native_count_) % 2] = item;
    ++native_count_;
    return true;
#else
    if (xQueueSend(s_action_queue, &item, 0) == pdPASS) {
        return true;
    }
    release_operation(operation);
    return false;
#endif
}

void Controller::process(Action action) {
    if (runtime_ == nullptr || backend_ == nullptr) {
        release_operation(action.operation);
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
    const usb_lifecycle::Snapshot current = state.usb_lifecycle_snapshot();
    const bool expected_lifecycle_state =
        action.kind == usb_lifecycle::ExecutorAction::kInstall
            ? current.desired == usb_lifecycle::DesiredExposure::kExposed &&
                  current.observed == usb_lifecycle::ObservedState::kAttaching
            : current.desired == usb_lifecycle::DesiredExposure::kHidden &&
                  current.observed == usb_lifecycle::ObservedState::kDetaching;
    if (active_operation_.load(std::memory_order_acquire) != action.operation ||
        !expected_lifecycle_state ||
        current.desired != action.snapshot.desired ||
        current.observed != action.snapshot.observed ||
        current.generation != action.snapshot.generation) {
        release_operation(action.operation);
        return;
    }
    if (action.kind == usb_lifecycle::ExecutorAction::kInstall) {
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
        state.mark_lifecycle_detach_uncertain(action.snapshot.generation);
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
