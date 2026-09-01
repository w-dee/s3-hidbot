#include <cassert>
#include <cstdint>

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

struct AcceptingExecutor final : usb_lifecycle::Executor {
    bool schedule(usb_lifecycle::ExecutorAction, usb_lifecycle::Snapshot) override {
        ++calls;
        return true;
    }
    int calls = 0;
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
}
