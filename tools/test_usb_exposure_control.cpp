#include <cassert>
#include <cstdint>
#include <vector>

#include "usb_exposure_control/usb_exposure_control.hpp"

namespace {

struct FakeBackend final : usb_exposure_control::Backend {
    usb_exposure_control::BackendResult install() override {
        ++install_calls;
        return install_result;
    }

    usb_exposure_control::BackendResult uninstall() override {
        ++uninstall_calls;
        return uninstall_result;
    }

    int install_calls = 0;
    int uninstall_calls = 0;
    usb_exposure_control::BackendResult install_result{
        .kind = usb_exposure_control::BackendResultKind::kSuccess,
        .error_code = 0,
    };
    usb_exposure_control::BackendResult uninstall_result{
        .kind = usb_exposure_control::BackendResultKind::kSuccess,
        .error_code = 0,
    };
};

void test_install_and_uninstall_are_task_owned_and_serialized() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    usb_exposure_control::Controller controller;
    assert(controller.initialize(&runtime, &backend));

    const auto cold = controller.snapshot();
    assert(cold.lifecycle.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(cold.lifecycle.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(backend.install_calls == 0 && backend.uninstall_calls == 0);

    assert(controller.request_attach() == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.request_attach() == usb_lifecycle::TransitionResult::kNoOp);
    assert(backend.install_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 1);
    assert(controller.snapshot().lifecycle.observed ==
           usb_lifecycle::ObservedState::kDisconnected);
    assert(!controller.process_one_for_test());

    runtime.state_machine().on_mount();
    runtime.state_machine().set_ready(hid_runtime::Interface::kKeyboard, true);
    runtime.state_machine().set_ready(hid_runtime::Interface::kMouse, true);
    const auto old_generation = controller.snapshot().lifecycle.generation;
    assert(controller.request_detach() == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.snapshot().lifecycle.generation == old_generation);
    assert(controller.request_detach() == usb_lifecycle::TransitionResult::kNoOp);
    assert(backend.uninstall_calls == 0);
    assert(controller.process_one_for_test());
    assert(backend.uninstall_calls == 1);
    const auto hidden = controller.snapshot();
    assert(hidden.lifecycle.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(hidden.lifecycle.generation == old_generation + 1);
    assert(!hidden.lifecycle.recovery_required);
}

void test_queue_is_bounded_and_does_not_drop_a_queued_action() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    usb_exposure_control::Controller controller;
    assert(controller.initialize(&runtime, &backend));
    const auto snapshot = controller.snapshot().lifecycle;

    // The producer-facing state machine prevents duplicate accepted
    // transitions. This direct executor probe establishes the fixed queue
    // boundary independently, with no backend call until the task drains it.
    assert(controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(controller.schedule(usb_lifecycle::ExecutorAction::kUninstall, snapshot));
    assert(!controller.schedule(usb_lifecycle::ExecutorAction::kInstall, snapshot));
    assert(backend.install_calls == 0 && backend.uninstall_calls == 0);
    assert(controller.process_one_for_test());
    assert(controller.process_one_for_test());
    assert(backend.install_calls == 1 && backend.uninstall_calls == 1);
}

void test_backend_failure_results_reach_lifecycle_state() {
    hid_runtime::Runtime runtime;
    FakeBackend backend;
    usb_exposure_control::Controller controller;
    assert(controller.initialize(&runtime, &backend));

    backend.install_result = {
        .kind = usb_exposure_control::BackendResultKind::kCleanInstallFailure,
        .error_code = -12,
    };
    assert(controller.request_attach() == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    auto snapshot = controller.snapshot().lifecycle;
    assert(snapshot.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(!snapshot.recovery_required);
    assert(snapshot.last_error.present && snapshot.last_error.code == -12);

    backend.install_result = {
        .kind = usb_exposure_control::BackendResultKind::kAmbiguousInstallFailure,
        .error_code = -99,
    };
    assert(controller.request_attach() == usb_lifecycle::TransitionResult::kAccepted);
    assert(controller.process_one_for_test());
    snapshot = controller.snapshot().lifecycle;
    assert(snapshot.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(snapshot.recovery_required);
    assert(snapshot.last_error.present && snapshot.last_error.code == -99);
}

}  // namespace

int main() {
    test_install_and_uninstall_are_task_owned_and_serialized();
    test_queue_is_bounded_and_does_not_drop_a_queued_action();
    test_backend_failure_results_reach_lifecycle_state();
}
