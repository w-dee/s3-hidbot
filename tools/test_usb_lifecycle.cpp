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

    void schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override {
        calls.push_back({action, snapshot});
    }

    std::vector<Call> calls;
};

void test_current_boot_policy_and_callbacks() {
    usb_lifecycle::StateMachine lifecycle;
    const auto boot = lifecycle.snapshot();
    assert(boot.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(boot.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(!lifecycle.accepts_hid(true));

    assert(lifecycle.observe_unmount());
    const auto disconnected_generation = lifecycle.generation();
    assert(lifecycle.observe_mount());
    assert(lifecycle.generation() != disconnected_generation);
    assert(lifecycle.accepts_hid(true));

    assert(lifecycle.observe_suspend());
    assert(!lifecycle.accepts_hid(true));
    assert(lifecycle.observe_resume());
    assert(lifecycle.accepts_hid(true));
}

void test_detach_is_intent_not_callback_and_preserves_uncertainty() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    lifecycle.observe_mount();
    const auto mounted_generation = lifecycle.generation();

    lifecycle.mark_release_uncertain();
    lifecycle.request_detach(executor);
    const auto detaching = lifecycle.snapshot();
    assert(detaching.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(detaching.observed == usb_lifecycle::ObservedState::kDetaching);
    assert(detaching.safety_pending);
    assert(detaching.host_release_uncertain);
    assert(detaching.generation != mounted_generation);
    assert(executor.calls.size() == 1);
    assert(executor.calls[0].action == usb_lifecycle::ExecutorAction::kDisconnect);

    // A late mount cannot reverse an already-linearized hidden intent.
    assert(!lifecycle.observe_mount());
    assert(lifecycle.snapshot().observed == usb_lifecycle::ObservedState::kDetaching);
    assert(lifecycle.observe_unmount());
    const auto hidden = lifecycle.snapshot();
    assert(hidden.observed == usb_lifecycle::ObservedState::kDisconnected);
    assert(hidden.host_release_uncertain);
    assert(!lifecycle.accepts_hid(true));
}

void test_attach_executor_and_wrap_boundary() {
    usb_lifecycle::StateMachine lifecycle;
    FakeExecutor executor;
    lifecycle.set_generation_for_test(UINT32_MAX);
    lifecycle.request_attach(executor);
    assert(lifecycle.generation() == 0);
    assert(executor.calls.size() == 1);
    assert(executor.calls[0].action == usb_lifecycle::ExecutorAction::kInstallAndConnect);
    assert(lifecycle.snapshot().observed == usb_lifecycle::ObservedState::kAttaching);
    assert(lifecycle.observe_mount());
    assert(lifecycle.generation() == 1);
}

void test_release_confirmation_is_explicit() {
    usb_lifecycle::StateMachine lifecycle;
    lifecycle.mark_release_pending();
    assert(lifecycle.snapshot().safety_pending);
    lifecycle.mark_release_uncertain();
    assert(lifecycle.snapshot().host_release_uncertain);
    lifecycle.mark_release_confirmed();
    const auto clean = lifecycle.snapshot();
    assert(!clean.safety_pending);
    assert(!clean.host_release_uncertain);
}

}  // namespace

int main() {
    test_current_boot_policy_and_callbacks();
    test_detach_is_intent_not_callback_and_preserves_uncertainty();
    test_attach_executor_and_wrap_boundary();
    test_release_confirmation_is_explicit();
}
