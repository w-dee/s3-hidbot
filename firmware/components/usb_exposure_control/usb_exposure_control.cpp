#include "usb_exposure_control/usb_exposure_control.hpp"

#ifndef USB_EXPOSURE_CONTROL_NATIVE_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#endif

namespace usb_exposure_control {
namespace {

#ifndef USB_EXPOSURE_CONTROL_NATIVE_TEST
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
#ifndef USB_EXPOSURE_CONTROL_NATIVE_TEST
    s_action_queue = xQueueCreateStatic(kActionQueueDepth, sizeof(Action), s_queue_bytes,
                                        &s_queue_storage);
    if (s_action_queue == nullptr ||
        xTaskCreateStatic(task_entry, "usb_lifecycle", kLifecycleTaskStackDepth,
                          this, kLifecycleTaskPriority, s_task_stack, &s_task_storage) == nullptr) {
        return false;
    }
#endif
    initialized_ = true;
    return true;
}

usb_lifecycle::TransitionResult Controller::request_attach() {
    if (!initialized_) {
        return usb_lifecycle::TransitionResult::kBusy;
    }
    return runtime_->state_machine().request_usb_attach(*this);
}

usb_lifecycle::TransitionResult Controller::request_detach() {
    if (!initialized_) {
        return usb_lifecycle::TransitionResult::kBusy;
    }
    return runtime_->state_machine().request_usb_detach(*this);
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
    if (!initialized_) {
        return false;
    }
    const Action item{.kind = action, .snapshot = snapshot};
#ifdef USB_EXPOSURE_CONTROL_NATIVE_TEST
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

void Controller::process(Action action) {
    if (runtime_ == nullptr || backend_ == nullptr) {
        return;
    }
    hid_runtime::StateMachine &state = runtime_->state_machine();
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
        return;
    }

#ifdef USB_EXPOSURE_CONTROL_NATIVE_TEST
    const hid_runtime::LifecycleSafetyResult safety = state.begin_lifecycle_detach_safety();
    if (safety != hid_runtime::LifecycleSafetyResult::kClean) {
        state.mark_lifecycle_detach_uncertain(action.snapshot.generation);
    }
#else
    (void)runtime_->run_lifecycle_detach_safety();
#endif
    state.begin_usb_uninstall();
    const BackendResult result = backend_->uninstall();
    if (result.kind == BackendResultKind::kSuccess) {
        state.on_driver_uninstalled();
        state.complete_usb_uninstall_success();
    } else {
        state.complete_usb_uninstall_failure(result.error_code);
    }
}

#ifdef USB_EXPOSURE_CONTROL_NATIVE_TEST
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

}  // namespace usb_exposure_control
