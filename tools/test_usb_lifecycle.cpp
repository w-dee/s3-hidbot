#include <cassert>
#include <cstdint>
#include <vector>

#include "usb_lifecycle/usb_lifecycle.hpp"

namespace {

struct FakeExecutor final : usb_lifecycle::Executor {
    struct Call {
        usb_lifecycle::ExecutorAction action;
        usb_lifecycle::Snapshot snapshot;
    };

    bool schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override {
        ++schedule_attempts;
        if (!accept) {
            return false;
        }
        calls.push_back({action, snapshot});
        return true;
    }

    bool accept = true;
    int schedule_attempts = 0;
    std::vector<Call> calls;
};

void test_cold_boot_is_hidden_without_action() {
    usb_lifecycle::StateMachine lifecycle;
    const auto boot = lifecycle.snapshot();
    assert(boot.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(boot.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(boot.generation == 0);
    assert(!boot.safety_pending);
    assert(!boot.host_release_uncertain);
    assert(!boot.recovery_required);
    assert(!boot.last_error.present);
    assert(!lifecycle.accepts_hid(true));
    assert(!lifecycle.observe_mount());
    assert(!lifecycle.observe_unmount());
}

void test_attach_install_and_mount_generation_semantics() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;

    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    assert(executor.calls.size() == 1);
    assert(executor.calls[0].action == usb_lifecycle::ExecutorAction::kInstall);
    const auto attaching = lifecycle.snapshot();
    assert(attaching.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(attaching.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(attaching.generation == 1);
    assert(executor.calls[0].snapshot.generation == attaching.generation);
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kNoOp);
    assert(executor.calls.size() == 1);

    lifecycle.complete_install_success();
    assert(lifecycle.snapshot().observed == usb_lifecycle::ObservedState::kDisconnected);
    const auto disconnected_generation = lifecycle.generation();
    assert(lifecycle.observe_mount());
    assert(lifecycle.snapshot().observed == usb_lifecycle::ObservedState::kMounted);
    assert(lifecycle.generation() == disconnected_generation);
    assert(lifecycle.accepts_hid(true));
    assert(lifecycle.observe_suspend());
    assert(!lifecycle.accepts_hid(true));
    assert(lifecycle.observe_resume());
    assert(lifecycle.accepts_hid(true));
}

void test_two_stage_detach_and_callback_reconciliation() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    lifecycle.complete_install_success();
    assert(lifecycle.observe_mount());
    const auto old_generation = lifecycle.generation();
    lifecycle.mark_release_pending();

    assert(lifecycle.request_detach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    const auto detaching = lifecycle.snapshot();
    assert(detaching.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(detaching.observed == usb_lifecycle::ObservedState::kDetaching);
    assert(detaching.safety_pending);
    assert(detaching.generation == old_generation);
    assert(executor.calls.size() == 2);
    assert(executor.calls.back().action == usb_lifecycle::ExecutorAction::kUninstall);
    assert(!lifecycle.observe_mount());
    assert(!lifecycle.observe_unmount());
    assert(lifecycle.generation() == old_generation);

    lifecycle.mark_release_uncertain_for_generation(old_generation);
    assert(lifecycle.begin_uninstall() == old_generation + 1);
    assert(lifecycle.begin_uninstall() == old_generation + 1);
    assert(lifecycle.generation() == old_generation + 1);
    lifecycle.complete_uninstall_success();
    const auto hidden = lifecycle.snapshot();
    assert(hidden.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(hidden.host_release_uncertain);
    assert(hidden.safety_pending);
    assert(hidden.uncertainty_generation == old_generation);
}

void test_failure_classification_and_new_retry() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    const auto first_generation = lifecycle.generation();
    lifecycle.complete_install_clean_failure(-12);
    const auto clean_failure = lifecycle.snapshot();
    assert(clean_failure.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(clean_failure.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(!clean_failure.recovery_required);
    assert(clean_failure.last_error.present);
    assert(clean_failure.last_error.operation == usb_lifecycle::LifecycleOperation::kInstall);
    assert(clean_failure.last_error.code == -12);
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    assert(lifecycle.generation() == first_generation + 1);

    lifecycle.complete_install_ambiguous_failure(-99);
    const auto ambiguous_failure = lifecycle.snapshot();
    assert(ambiguous_failure.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(ambiguous_failure.recovery_required);
    assert(ambiguous_failure.last_error.present);
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kBusy);
    assert(lifecycle.request_detach(executor) == usb_lifecycle::TransitionResult::kBusy);
}

void test_uninstall_failure_is_explicit_recovery_state() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    lifecycle.complete_install_success();
    assert(lifecycle.request_detach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    lifecycle.mark_release_confirmed();
    lifecycle.begin_uninstall();
    lifecycle.complete_uninstall_failure(-7);
    const auto failed = lifecycle.snapshot();
    assert(failed.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(failed.observed == usb_lifecycle::ObservedState::kDetaching);
    assert(failed.recovery_required);
    assert(failed.last_error.present);
    assert(failed.last_error.operation == usb_lifecycle::LifecycleOperation::kUninstall);
    assert(failed.last_error.code == -7);
}

void test_external_unmount_advances_once_and_later_mount_reuses_generation() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    lifecycle.complete_install_success();
    assert(lifecycle.observe_mount());
    const auto mounted_generation = lifecycle.generation();
    assert(lifecycle.observe_unmount());
    assert(lifecycle.snapshot().observed == usb_lifecycle::ObservedState::kDisconnected);
    assert(lifecycle.generation() == mounted_generation + 1);
    assert(lifecycle.observe_mount());
    assert(lifecycle.generation() == mounted_generation + 1);
}

void test_noops_conflicts_and_wrap_boundary() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    assert(lifecycle.request_detach(executor) == usb_lifecycle::TransitionResult::kNoOp);
    lifecycle.set_generation_for_test(UINT32_MAX);
    assert(lifecycle.request_attach(executor) == usb_lifecycle::TransitionResult::kAccepted);
    assert(lifecycle.generation() == 0);
    assert(lifecycle.request_detach(executor) == usb_lifecycle::TransitionResult::kBusy);
}

}  // namespace

int main() {
    test_cold_boot_is_hidden_without_action();
    test_attach_install_and_mount_generation_semantics();
    test_two_stage_detach_and_callback_reconciliation();
    test_failure_classification_and_new_retry();
    test_uninstall_failure_is_explicit_recovery_state();
    test_external_unmount_advances_once_and_later_mount_reuses_generation();
    test_noops_conflicts_and_wrap_boundary();
}
