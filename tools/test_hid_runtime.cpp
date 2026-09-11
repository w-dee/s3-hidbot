#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

#include "hid_runtime/hid_runtime.hpp"

namespace {

struct Sink {
    int calls = 0;
    std::uint8_t instance = 0;
    std::uint16_t length = 0;
    std::array<std::uint8_t, 8> report{};
    bool accept = true;

    static bool submit(void *context, std::uint8_t instance,
                       const std::uint8_t *report, std::uint16_t length) {
        auto *sink = static_cast<Sink *>(context);
        ++sink->calls;
        sink->instance = instance;
        sink->length = length;
        std::memcpy(sink->report.data(), report, length);
        return sink->accept;
    }
};

struct ImmediateLifecycleExecutor final : usb_lifecycle::Executor {
    bool schedule(usb_lifecycle::ExecutorAction,
                  usb_lifecycle::Snapshot) override {
        return true;
    }
};

usb_lifecycle::TransitionResult action(hid_runtime::UsbTransitionOutcome outcome) {
    return outcome.action_result;
}

void expose(hid_runtime::StateMachine &state) {
    ImmediateLifecycleExecutor executor;
    assert(action(state.request_usb_attach(executor)) == usb_lifecycle::TransitionResult::kAccepted);
    state.complete_usb_install_success();
    state.on_mount();
}

void ready(hid_runtime::StateMachine &state) {
    expose(state);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
}

std::atomic_bool transition_hook_entered{false};
std::atomic_bool transition_contender_started{false};
std::atomic_bool transition_contender_finished{false};

void reset_transition_probe() {
    transition_hook_entered.store(false, std::memory_order_relaxed);
    transition_contender_started.store(false, std::memory_order_relaxed);
    transition_contender_finished.store(false, std::memory_order_relaxed);
}

void hold_exact_ticket_transition(hid_runtime::StateMachine *) {
    transition_hook_entered.store(true, std::memory_order_release);
    while (!transition_contender_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // The competing begin operation has reached the ticket metadata lock. It
    // cannot reap or replace this ticket until the exact transition commits.
    assert(!transition_contender_finished.load(std::memory_order_acquire));
}

std::atomic_bool terminal_hook_entered{false};
std::atomic_bool terminal_observer_started{false};
std::atomic_bool terminal_observer_finished{false};

void reset_terminal_probe() {
    terminal_hook_entered.store(false, std::memory_order_relaxed);
    terminal_observer_started.store(false, std::memory_order_relaxed);
    terminal_observer_finished.store(false, std::memory_order_relaxed);
}

void hold_before_terminal_publication(hid_runtime::StateMachine *) {
    terminal_hook_entered.store(true, std::memory_order_release);
    while (!terminal_observer_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Snapshot must remain blocked until outcome and terminal state are both
    // published by the same metadata transaction.
    assert(!terminal_observer_finished.load(std::memory_order_acquire));
}

enum class WritingProbeInterface { kKeyboard, kMouse };

WritingProbeInterface writing_probe_interface = WritingProbeInterface::kKeyboard;
hid_runtime::HidTicketId writing_probe_ticket = 0;
hid_runtime::KeyboardReportBeginResult writing_probe_keyboard_result =
    hid_runtime::KeyboardReportBeginResult::kNotReady;
hid_runtime::MouseReportBeginResult writing_probe_mouse_result =
    hid_runtime::MouseReportBeginResult::kNotReady;

void cancel_writer_owned_ticket(hid_runtime::StateMachine *state) {
    state->set_before_ticket_publish_hook_for_test(nullptr);
    if (writing_probe_interface == WritingProbeInterface::kKeyboard) {
        const auto writing = state->keyboard_report_snapshot();
        assert(writing.state ==
               hid_runtime::KeyboardReportTicketState::kWriting);
        writing_probe_ticket = writing.ticket_id;
        state->begin_release_all();
        hid_runtime::KeyboardReportSnapshot canceled{};
        assert(state->keyboard_report_snapshot(writing_probe_ticket, &canceled));
        assert(canceled.state ==
               hid_runtime::KeyboardReportTicketState::kWritingCanceled);
        writing_probe_keyboard_result = state->begin_keyboard_report(
            0, {5, 0, 0, 0, 0, 0});
    } else {
        const auto writing = state->mouse_report_snapshot();
        assert(writing.state == hid_runtime::MouseReportTicketState::kWriting);
        writing_probe_ticket = writing.ticket_id;
        state->begin_release_all();
        hid_runtime::MouseReportSnapshot canceled{};
        assert(state->mouse_report_snapshot(writing_probe_ticket, &canceled));
        assert(canceled.state ==
               hid_runtime::MouseReportTicketState::kWritingCanceled);
        writing_probe_mouse_result =
            state->begin_mouse_report(0, 2, 0, 0, 0);
    }
}

void test_lifecycle_and_generation_cancellation() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::uint32_t first_generation = state.attach_generation();
    assert(first_generation != 0);
    assert(state.status().mounted);

    assert(state.queue_mouse_report(1, 10, 0, 0, 0));
    state.cancel_queued(hid_runtime::Interface::kMouse);
    Sink canceled_sink;
    state.execute(Sink::submit, &canceled_sink);
    assert(canceled_sink.calls == 0);
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    state.on_unmount();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.attach_generation() != first_generation);
    assert(!state.status().suspended);

    state.on_suspend();
    assert(state.status().suspended);
    assert(!state.queue_mouse_report(0, 10, 0, 0, 0));
    state.on_resume();
    state.set_ready(hid_runtime::Interface::kMouse, true);
}

void test_readiness_refresh_after_mount_without_hid_work() {
    hid_runtime::StateMachine state;

    // Runtime::initialize() and the initial state both represent an
    // unmounted device with no endpoint readiness.
    assert(!state.status().mounted);
    assert(!state.status().keyboard_ready);
    assert(!state.status().mouse_ready);

    expose(state);
    auto snapshot = state.status();
    assert(snapshot.mounted && !snapshot.suspended);
    assert(!snapshot.keyboard_ready && !snapshot.mouse_ready);

    // The native test models the production service_sof() observation with
    // set_ready(): the backend can still be unavailable immediately after
    // mount, and no HID mailbox operation is needed for readiness to change.
    state.set_ready(hid_runtime::Interface::kKeyboard, false);
    state.set_ready(hid_runtime::Interface::kMouse, false);
    snapshot = state.status();
    assert(!snapshot.keyboard_ready && !snapshot.mouse_ready);

    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    snapshot = state.status();
    assert(snapshot.mounted && !snapshot.suspended);
    assert(snapshot.keyboard_ready && snapshot.mouse_ready);
}

void test_sequence_admission_snapshot_and_producer_exclusion() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    std::array<std::uint8_t, 6> held{4, 0, 0, 0, 0, 0};
    assert(state.begin_keyboard_report(1, held) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    assert(state.report_complete(0, sink.report.data(), 8));

    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    assert(snapshot.keyboard.modifiers == 1);
    assert(snapshot.keyboard.keycodes == held);
    assert(snapshot.mouse.buttons == 0);
    assert(sequence.authority_epoch == state.authority_epoch());
    assert(state.sequence_active());
    assert(state.begin_keyboard_report(0, {}) ==
           hid_runtime::KeyboardReportBeginResult::kBusy);
    assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kBusy);
    assert(state.begin_keyboard_report(0, {}, sequence) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.end_sequence(sequence);
    assert(!state.sequence_active());
}

void test_revoked_sequence_authority_cannot_create_or_submit_ticket() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    state.request_release_all();
    assert(!state.sequence_authority_current(sequence));
    assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, sequence) ==
           hid_runtime::KeyboardReportBeginResult::kAuthorityLost);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);

    // Lifecycle/session authority retirement rejects the same stale owner.
    state.finalize_release_all();
    hid_runtime::SequenceAuthority replacement{};
    assert(state.begin_sequence(&snapshot, &replacement) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    state.on_suspend();
    assert(state.begin_mouse_report(1, 0, 0, 0, 0, replacement) ==
           hid_runtime::MouseReportBeginResult::kAuthorityLost);
}

void revoke_sequence_while_ticket_is_writing(hid_runtime::StateMachine *state) {
    state->begin_release_all();
}

void test_sequence_ticket_paused_before_publication_cannot_survive_release() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    state.set_before_ticket_publish_hook_for_test(
        revoke_sequence_while_ticket_is_writing);
    assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, sequence) ==
           hid_runtime::KeyboardReportBeginResult::kAuthorityLost);
    state.set_before_ticket_publish_hook_for_test(nullptr);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(!state.sequence_authority_current(sequence));
    assert((state.keyboard_state().keycodes ==
            std::array<std::uint8_t, 6>{}));
    const auto release = state.release_all_snapshot();
    assert(release.keyboard ==
           hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    assert(release.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    state.finalize_release_all();
}

void test_sequence_generation_does_not_wrap_to_stale_authority() {
    hid_runtime::StateMachine state;
    ready(state);
    state.set_next_sequence_generation_for_test(
        std::numeric_limits<std::uint32_t>::max());
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority final_authority{};
    assert(state.begin_sequence(&snapshot, &final_authority) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    assert(final_authority.generation ==
           std::numeric_limits<std::uint32_t>::max());
    state.end_sequence(final_authority);
    hid_runtime::SequenceAuthority exhausted{};
    assert(state.begin_sequence(&snapshot, &exhausted) ==
           hid_runtime::SequenceAdmissionResult::kNotReady);
}

hid_runtime::SequenceAdmissionResult ble_admission_during_commit;
hid_runtime::SequenceAdmissionResult ble_admission_before_acceptance;

void attempt_sequence_during_ble_commit(hid_runtime::StateMachine *state) {
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    ble_admission_during_commit = state->begin_sequence(&snapshot, &sequence);
    if (ble_admission_during_commit ==
        hid_runtime::SequenceAdmissionResult::kAccepted) {
        state->end_sequence(sequence);
    }
}

void attempt_sequence_before_ble_acceptance(hid_runtime::StateMachine *state) {
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    ble_admission_before_acceptance = state->begin_sequence(&snapshot, &sequence);
    if (ble_admission_before_acceptance ==
        hid_runtime::SequenceAdmissionResult::kAccepted) {
        state->end_sequence(sequence);
    }
}

hid_runtime::BleSubmitResult accept_ble_report(
    void *, hid_runtime::Interface, hid_runtime::HidWorkToken,
    const std::uint8_t *, std::uint16_t) {
    return hid_runtime::BleSubmitResult::kStackAccepted;
}

void make_ble_ready(hid_runtime::StateMachine &state) {
    const auto route = state.route_snapshot();
    assert(state.request_route_ble({
        .expected_authority_epoch = state.authority_epoch(),
        .expected_route_generation = route.generation,
        .ble_generation = 11,
        .connection_handle = 12,
        .keyboard_characteristic_handle = 13,
        .mouse_characteristic_handle = 14,
    }).action_result == hid_runtime::RouteTransitionResult::kAccepted);
}

void test_ble_terminal_visibility_follows_confirmed_state() {
    for (const hid_runtime::Interface interface :
         {hid_runtime::Interface::kKeyboard, hid_runtime::Interface::kMouse}) {
        hid_runtime::StateMachine state;
        make_ble_ready(state);
        if (interface == hid_runtime::Interface::kKeyboard) {
            assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
                   hid_runtime::KeyboardReportBeginResult::kPublished);
        } else {
            assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
                   hid_runtime::MouseReportBeginResult::kPublished);
        }
        const hid_runtime::HidWorkToken token =
            state.published_report_token(interface);
        assert(state.mark_ble_report_scheduled(interface, token));
        state.set_before_submit_hook_for_test(
            attempt_sequence_before_ble_acceptance);
        state.set_before_ble_terminal_publish_hook_for_test(
            attempt_sequence_during_ble_commit);
        assert(state.process_ble_report(interface, token, accept_ble_report,
                                        nullptr));
        state.set_before_submit_hook_for_test(nullptr);
        state.set_before_ble_terminal_publish_hook_for_test(nullptr);
        assert(ble_admission_before_acceptance ==
               hid_runtime::SequenceAdmissionResult::kBusy);
        assert(ble_admission_during_commit ==
               hid_runtime::SequenceAdmissionResult::kBusy);
        if (interface == hid_runtime::Interface::kKeyboard) {
            state.finalize_keyboard_report();
        } else {
            state.finalize_mouse_report();
        }
        hid_runtime::ConfirmedHidState snapshot{};
        hid_runtime::SequenceAuthority sequence{};
        assert(state.begin_sequence(&snapshot, &sequence) ==
               hid_runtime::SequenceAdmissionResult::kAccepted);
        assert(interface == hid_runtime::Interface::kKeyboard
                   ? snapshot.keyboard.keycodes[0] == 4
                   : snapshot.mouse.buttons == 1);
        state.end_sequence(sequence);
    }
}

hid_runtime::SequenceAdmissionResult mailbox_race_admission;

void attempt_sequence_while_mailbox_claimed(hid_runtime::StateMachine *state) {
    state->set_before_submit_hook_for_test(nullptr);
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    mailbox_race_admission = state->begin_sequence(&snapshot, &sequence);
    if (mailbox_race_admission ==
        hid_runtime::SequenceAdmissionResult::kAccepted) {
        state->end_sequence(sequence);
    }
}

void test_mailbox_sequence_exclusion_and_safety_priority() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    hid_runtime::ConfirmedHidState snapshot{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    assert(!state.queue_mouse_report(0, 10, 0, 0, 0));
    state.end_sequence(sequence);

    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kBusy);
    state.set_before_submit_hook_for_test(attempt_sequence_while_mailbox_claimed);
    state.execute(Sink::submit, &sink);
    assert(mailbox_race_admission == hid_runtime::SequenceAdmissionResult::kBusy);
    assert(sink.calls == 1);
    assert(state.report_complete(1, sink.report.data(), 5));

    assert(state.begin_sequence(&snapshot, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);
    assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, sequence) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2);
    assert(state.report_complete(0, sink.report.data(), 8));
    state.finalize_keyboard_report();
    state.request_release_all();
    assert(!state.sequence_authority_current(sequence));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 0 && sink.report[0] == 0 &&
           sink.report[2] == 0);
}

void test_readiness_refresh_after_reattach() {
    hid_runtime::StateMachine state;

    expose(state);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    const std::uint32_t first_generation = state.attach_generation();
    assert(state.status().keyboard_ready && state.status().mouse_ready);

    state.on_unmount();
    auto snapshot = state.status();
    assert(!snapshot.mounted && !snapshot.keyboard_ready && !snapshot.mouse_ready);

    state.on_mount();
    snapshot = state.status();
    assert(state.attach_generation() != first_generation);
    assert(snapshot.mounted && !snapshot.suspended);
    assert(!snapshot.keyboard_ready && !snapshot.mouse_ready);

    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    snapshot = state.status();
    assert(snapshot.keyboard_ready && snapshot.mouse_ready);
}

void invalidate_route_before_submit(hid_runtime::StateMachine *state) {
    state->on_suspend();
}

void test_route_generation_is_independent_and_gates_stale_unsafe_work() {
    hid_runtime::StateMachine state;
    Sink sink;
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.route_snapshot().generation == 0);

    expose(state);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    const hid_route::Snapshot usb_route = state.route_snapshot();
    assert(usb_route.active == hid_route::OutputRoute::kUsb);
    assert(usb_route.generation == 1);

    assert(state.queue_mouse_report(0, 1, 0, 0, 0));
    state.set_before_submit_hook_for_test(invalidate_route_before_submit);
    state.execute(Sink::submit, &sink);
    state.set_before_submit_hook_for_test(nullptr);
    assert(sink.calls == 0);
    const hid_route::Snapshot invalidated = state.route_snapshot();
    assert(invalidated.active == hid_route::OutputRoute::kNone);
    assert(invalidated.generation == usb_route.generation + 1);
    assert(!state.queue_mouse_report(0, 1, 0, 0, 0));
}

void test_usb_transition_outcomes_freeze_stage_a_runtime() {
    hid_runtime::StateMachine state;
    ImmediateLifecycleExecutor executor;

    const auto attach = state.request_usb_attach(executor);
    assert(attach.action_result == usb_lifecycle::TransitionResult::kAccepted);
    assert(attach.snapshot_valid);
    assert(attach.lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(attach.lifecycle.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(attach.lifecycle.generation == 1);
    assert(!attach.runtime.mounted && !attach.runtime.suspended);
    assert(!attach.runtime.keyboard_ready && !attach.runtime.mouse_ready);

    state.complete_usb_install_success();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    const auto detach = state.request_usb_detach(executor);
    assert(detach.action_result == usb_lifecycle::TransitionResult::kAccepted);
    assert(detach.snapshot_valid);
    assert(detach.lifecycle.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(detach.lifecycle.observed == usb_lifecycle::ObservedState::kDetaching);
    assert(detach.lifecycle.generation == 1);
    assert(detach.runtime.mounted && !detach.runtime.suspended);
    assert(detach.runtime.keyboard_ready && detach.runtime.mouse_ready);
}

void test_success_failure_and_release() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    assert(state.queue_mouse_report(3, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.instance == 1 && sink.length == 5);
    assert(sink.report[0] == 3 && sink.report[1] == 10);
    assert(state.report_in_flight(hid_runtime::Interface::kMouse));
    state.request_release_all();
    state.execute(Sink::submit, &sink);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    state.report_complete(1);
    assert(state.mouse_state().buttons == 3);

    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.report[0] == 0 && sink.report[1] == 0);
    state.report_complete(1);
    assert(state.mouse_state().buttons == 0);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));

    assert(state.queue_mouse_report(1, 1, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    state.report_failed(1);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    assert(state.host_state_uncertain(hid_runtime::Interface::kMouse));
    state.execute(Sink::submit, &sink);
    assert(sink.report[0] == 0 && sink.report[1] == 0);
    sink.accept = false;
    state.report_failed(1);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    sink.accept = true;
    state.execute(Sink::submit, &sink);
    state.report_complete(1);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
}

void test_release_all_noop_for_known_all_up() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // Relative movement does not persist as held state.  An all-up report
    // already queued but not submitted is discarded without a needless
    // safety report.
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    state.request_release_all();
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
}

void test_zero_work_release_terminalizes_lifecycle_pending() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // This is the same producer-side primitive used by session takeover.
    state.request_release_all();
    assert(state.usb_lifecycle_snapshot().safety_pending);
    assert(state.release_requested_for_test());
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    const auto reconciled = state.usb_lifecycle_snapshot();
    assert(!reconciled.safety_pending);
    assert(!reconciled.host_release_uncertain);
    assert(!state.release_requested_for_test());

    // The public operation still reports the unchanged per-interface state.
    state.begin_release_all();
    const auto release = state.release_all_snapshot();
    assert(release.keyboard == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    assert(release.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    state.finalize_release_all();
    assert(state.usb_lifecycle_snapshot().safety_pending);
    state.execute(Sink::submit, &sink);
    assert(!state.usb_lifecycle_snapshot().safety_pending);
}

void test_hidden_clean_release_terminalizes_without_executor() {
    hid_runtime::StateMachine state;
    Sink sink;

    const auto cold = state.usb_lifecycle_snapshot();
    assert(cold.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(cold.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(cold.generation == 0);
    assert(!cold.safety_pending && !cold.host_release_uncertain && !cold.recovery_required);

    state.request_release_all();
    const auto reconciled = state.usb_lifecycle_snapshot();
    assert(reconciled.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(reconciled.observed == usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(reconciled.generation == 0);
    assert(!reconciled.safety_pending && !reconciled.host_release_uncertain);
    assert(!state.release_requested_for_test());
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
}

void test_disconnected_clean_release_terminalizes_without_executor() {
    hid_runtime::StateMachine state;
    ImmediateLifecycleExecutor executor;
    Sink sink;

    assert(action(state.request_usb_attach(executor)) ==
           usb_lifecycle::TransitionResult::kAccepted);
    state.complete_usb_install_success();
    const auto disconnected = state.usb_lifecycle_snapshot();
    assert(disconnected.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(disconnected.observed == usb_lifecycle::ObservedState::kDisconnected);
    assert(!state.status().mounted);

    state.request_release_all();
    assert(!state.usb_lifecycle_snapshot().safety_pending);
    assert(!state.release_requested_for_test());
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
}

void publish_new_hidden_release_request(hid_runtime::StateMachine *state) {
    state->set_before_release_reconciliation_hook_for_test(nullptr);
    const std::uint32_t previous_epoch = state->release_request_epoch_for_test();
    state->publish_release_request_only_for_test();
    assert(state->release_requested_for_test());
    assert(state->release_request_epoch_for_test() == previous_epoch + 1);
    assert(state->usb_lifecycle_snapshot().safety_pending);
}

void test_new_hidden_request_survives_old_reconciliation() {
    hid_runtime::StateMachine state;
    state.set_before_release_reconciliation_hook_for_test(
        publish_new_hidden_release_request);

    state.request_release_all();
    assert(state.release_request_epoch_for_test() == 2);
    assert(!state.release_requested_for_test());
    assert(!state.usb_lifecycle_snapshot().safety_pending);
}

void attach_and_publish_new_release(hid_runtime::StateMachine *state) {
    state->set_before_release_reconciliation_hook_for_test(nullptr);
    ImmediateLifecycleExecutor executor;
    assert(action(state->request_usb_attach(executor)) ==
           usb_lifecycle::TransitionResult::kAccepted);
    state->publish_release_request_only_for_test();
    assert(state->release_requested_for_test());
    assert(state->usb_lifecycle_snapshot().safety_pending);
}

void test_attach_boundary_blocks_stale_hidden_reconciliation() {
    hid_runtime::StateMachine state;
    state.set_before_release_reconciliation_hook_for_test(attach_and_publish_new_release);

    state.request_release_all();
    const auto attaching = state.usb_lifecycle_snapshot();
    assert(attaching.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(attaching.observed == usb_lifecycle::ObservedState::kAttaching);
    assert(attaching.generation == 1);
    assert(state.authority_epoch() == 1);
    assert(state.release_requested_for_test());
    assert(attaching.safety_pending);

    state.complete_usb_install_success();
    state.on_mount();
    assert(!state.release_requested_for_test());
    assert(!state.usb_lifecycle_snapshot().safety_pending);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
}

void test_later_attach_after_clean_hidden_release_has_no_stale_work() {
    hid_runtime::StateMachine state;
    ImmediateLifecycleExecutor executor;
    Sink sink;

    state.request_release_all();
    assert(!state.release_requested_for_test());
    assert(!state.usb_lifecycle_snapshot().safety_pending);
    assert(action(state.request_usb_attach(executor)) ==
           usb_lifecycle::TransitionResult::kAccepted);
    state.complete_usb_install_success();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(!state.release_requested_for_test());
    assert(!state.usb_lifecycle_snapshot().safety_pending);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
}

void test_uncertainty_is_not_zero_work_terminalized() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // A relative report is logically all-up, but a failed transfer makes host
    // delivery genuinely uncertain and therefore requires a fresh safety report.
    assert(state.queue_mouse_report(0, 1, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(state.report_in_flight(hid_runtime::Interface::kMouse));
    assert(state.report_failed(1));
    assert(state.usb_lifecycle_snapshot().host_release_uncertain);

    state.request_release_all();
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2);
    const auto pending = state.usb_lifecycle_snapshot();
    assert(pending.safety_pending);
    assert(pending.host_release_uncertain);
}

void test_held_keyboard_release_requires_safety_completion() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> f24 = {0x73, 0, 0, 0, 0, 0};

    assert(state.queue_keyboard_report(0, f24));
    state.execute(Sink::submit, &sink);
    assert(state.report_complete(0));
    state.request_release_all();
    assert(state.usb_lifecycle_snapshot().safety_pending);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2);
    assert(state.report_in_flight(hid_runtime::Interface::kKeyboard));
    assert(state.usb_lifecycle_snapshot().safety_pending);
    assert(state.report_complete(0));
    assert(!state.safety_required(hid_runtime::Interface::kKeyboard));
    assert(!state.usb_lifecycle_snapshot().safety_pending);
}

void publish_new_release_request(hid_runtime::StateMachine *state) {
    state->request_release_all();
}

void test_new_release_request_survives_zero_work_reconciliation_race() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    state.request_release_all();
    state.set_before_release_reconciliation_hook_for_test(publish_new_release_request);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.usb_lifecycle_snapshot().safety_pending);

    state.set_before_release_reconciliation_hook_for_test(nullptr);
    state.execute(Sink::submit, &sink);
    assert(!state.usb_lifecycle_snapshot().safety_pending);
}

void test_partial_release_and_no_delayed_unsafe_replay() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(2, keys));
    state.execute(Sink::submit, &sink);
    state.report_complete(0);
    assert(state.keyboard_state().modifiers == 2);

    state.request_release_all();
    state.execute(Sink::submit, &sink);
    assert(sink.instance == 0 && sink.length == 8 && sink.report[0] == 0 && sink.report[2] == 0);
    state.report_complete(0);
    assert(!state.safety_required(hid_runtime::Interface::kKeyboard));
    assert(!state.safety_required(hid_runtime::Interface::kMouse));

    // Not-ready unsafe work is discarded and cannot appear after resume.
    state.set_ready(hid_runtime::Interface::kMouse, false);
    assert(!state.queue_mouse_report(0, 10, 0, 0, 0));
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2);
}

void test_release_barrier_discards_queued_work() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // A request in the producer task blocks new unsafe work immediately.  Any
    // report already in the mailbox is discarded by the TinyUSB executor and
    // is never replayed after the safety pass.
    assert(state.queue_mouse_report(1, 10, 0, 0, 0));
    state.request_release_all();
    assert(!state.queue_mouse_report(0, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.report[0] == 0 && sink.report[1] == 0);
    state.report_complete(1);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
}

void test_executor_submission_bound() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, keys));
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));

    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    const std::uint8_t first_instance = sink.instance;
    state.report_complete(first_instance);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance != first_instance);
}

void test_authority_epoch_suspend_resume_barrier() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::uint32_t attach_before_suspend = state.attach_generation();
    const hid_runtime::AuthorityEpoch epoch_before_suspend = state.authority_epoch();

    // A READY unsafe operation is canceled at suspend and cannot be replayed
    // merely because readiness becomes true again after resume.
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    state.on_suspend();
    const auto suspended = state.status();
    assert(suspended.mounted && suspended.suspended);
    assert(!suspended.keyboard_ready && !suspended.mouse_ready);
    assert(state.authority_epoch() != epoch_before_suspend);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);

    const hid_runtime::AuthorityEpoch epoch_after_suspend = state.authority_epoch();
    state.on_resume();
    assert(state.authority_epoch() != epoch_after_suspend);
    assert(state.attach_generation() == attach_before_suspend);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
}

void test_suspend_preserves_safety_and_ignores_late_completion() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(2, keys));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && state.report_in_flight(hid_runtime::Interface::kKeyboard));

    state.on_suspend();
    assert(state.safety_required(hid_runtime::Interface::kKeyboard));
    assert(state.host_state_uncertain(hid_runtime::Interface::kKeyboard));
    assert(!state.report_in_flight(hid_runtime::Interface::kKeyboard));
    state.on_resume();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);

    // Completion from the pre-suspend authority cannot clear the new epoch's
    // all-up requirement, and no unsafe operation may enter while it remains.
    assert(!state.report_complete(0));
    assert(!state.queue_mouse_report(0, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0 && sink.report[2] == 0);
    state.report_complete(0);
    assert(!state.safety_required(hid_runtime::Interface::kKeyboard));
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[1] == 10);
}

void test_unmount_preserves_uncertainty_for_fresh_generation_reconciliation() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.queue_mouse_report(1, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    state.on_suspend();
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    const std::uint32_t old_attach = state.attach_generation();
    const hid_runtime::AuthorityEpoch old_epoch = state.authority_epoch();
    state.on_unmount();
    assert(!state.status().mounted);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    assert(state.host_state_uncertain(hid_runtime::Interface::kMouse));
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.attach_generation() != old_attach);
    assert(state.authority_epoch() != old_epoch);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0);
    state.report_complete(0);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[0] == 0);
    state.report_complete(1);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
}

void test_release_ticket_states_and_historical_submission() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    state.begin_release_all();
    auto snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    // Even if the SOF owner consumes the epoch request before the public
    // caller reads/finalizes its clean result, the active release ticket is a
    // producer gate for that remaining window.
    state.execute(Sink::submit, &sink);
    assert(!state.release_requested_for_test());
    assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
           hid_runtime::KeyboardReportBeginResult::kSafetyPending);
    hid_runtime::ConfirmedHidState hid_state{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&hid_state, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kSafetyPending);
    state.finalize_release_all();

    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, keys));
    state.execute(Sink::submit, &sink);
    state.report_complete(0);
    state.begin_release_all();
    state.execute(Sink::submit, &sink);
    snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kSubmitted);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    // Completion is not allowed to rewrite this ticket's historical outcome.
    state.report_complete(0);
    snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kSubmitted);
    state.finalize_release_all();
}

void test_release_ticket_failure_and_lifecycle_cancellation() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.queue_mouse_report(1, 1, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    state.report_complete(1);

    state.begin_release_all();
    state.execute(Sink::submit, &sink);
    assert(state.report_in_flight(hid_runtime::Interface::kMouse));
    state.report_failed(1);
    auto snapshot = state.release_all_snapshot();
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kPending);
    assert(snapshot.failed_before_finalization);
    state.finalize_release_all();

    state.begin_release_all();
    const auto old_epoch = state.release_all_snapshot().authority_epoch;
    state.on_suspend();
    snapshot = state.release_all_snapshot();
    assert(snapshot.canceled);
    assert(snapshot.authority_epoch == old_epoch);
    assert(!snapshot.active);
}

void test_release_ticket_partial_and_clean_unmounted() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, keys));
    state.execute(Sink::submit, &sink);
    state.report_complete(0);
    assert(state.queue_mouse_report(1, 0, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    state.report_complete(1);
    state.set_ready(hid_runtime::Interface::kMouse, false);
    state.begin_release_all();
    auto snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kUnresolved);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kPending);
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    state.execute(Sink::submit, &sink);
    snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kSubmitted);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kPending);
    state.finalize_release_all();

    state.on_unmount();
    state.begin_release_all();
    snapshot = state.release_all_snapshot();
    // Unmount cannot prove either host-side all-up when an earlier safety
    // release was still incomplete. The public operation stays fail-closed.
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kPending);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kPending);
}

void test_keyboard_report_ticket_and_confirmed_state() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 5, 0xA4, 0xB0, 0xDD, 0};

    // The all-up report is unsafe even when the confirmed state is initially
    // all-up. It is published once, then resolved by the SOF executor.
    const std::array<std::uint8_t, 6> all_up{};
    assert(state.begin_keyboard_report(0, all_up) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(state.keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.instance == 0 && sink.length == 8);
    assert(state.keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kSubmitted);
    state.report_complete(0);
    state.finalize_keyboard_report();

    assert(state.begin_keyboard_report(2, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    // A second request while the first ticket is still published is busy.
    assert(state.begin_keyboard_report(2, keys) ==
           hid_runtime::KeyboardReportBeginResult::kBusy);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 2 &&
           sink.report[2] == 4 && sink.report[5] == 0xB0 && sink.report[6] == 0xDD);
    state.report_complete(0);
    state.finalize_keyboard_report();
    assert(state.begin_keyboard_report(2, keys) ==
           hid_runtime::KeyboardReportBeginResult::kAlreadySet);

    // U4.2 safety recovery still owns a confirmed held keyboard state that
    // originated through the public ticket path.
    state.begin_release_all();
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 0 && sink.report[0] == 0 &&
           sink.report[2] == 0);
    state.report_complete(0);
    state.finalize_release_all();

    // An in-flight report remains busy until its completion callback commits
    // the provisional state to the confirmed snapshot.
    assert(state.begin_keyboard_report(0, all_up) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(state.begin_keyboard_report(0, all_up) ==
           hid_runtime::KeyboardReportBeginResult::kBusy);
    state.report_complete(0);
    state.finalize_keyboard_report();
}

void test_keyboard_report_ticket_cancellation_and_barriers() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};

    // Timeout/control cancellation wins before SOF and cannot become a ghost
    // keypress on any later executor pass.
    assert(state.begin_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(state.cancel_keyboard_report(
        state.keyboard_report_snapshot().ticket_id));
    state.execute(Sink::submit, &sink);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    state.finalize_keyboard_report();

    // Lifecycle publication cancels a published ticket and advances the
    // authority barrier before the executor can claim it.
    assert(state.begin_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    const auto epoch = state.authority_epoch();
    state.on_suspend();
    assert(state.authority_epoch() != epoch);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.keyboard_report_snapshot().outcome ==
           hid_runtime::KeyboardReportTicketOutcome::kAuthorityLost);
    state.finalize_keyboard_report();
    state.on_resume();

    // A mouse safety requirement is global: keyboard unsafe work is rejected
    // until the all-up barrier completes.
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(state.queue_mouse_report(1, 0, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    state.request_release_all();
    assert(state.begin_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kSafetyPending);
}

void test_keyboard_report_submit_false_is_not_replayed() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    sink.accept = false;
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    assert(state.begin_keyboard_report(0, keys) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    assert(state.keyboard_report_snapshot().state ==
           hid_runtime::KeyboardReportTicketState::kNotReady);
    state.finalize_keyboard_report();
    sink.accept = true;
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    assert((state.keyboard_state().keycodes == std::array<std::uint8_t, 6>{}));
}

void test_mouse_report_ticket_relative_and_confirmed_state() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // A clean zero-delta report is already_set, but every nonzero relative
    // delta is a fresh operation even when the payload is unchanged.
    assert(state.begin_mouse_report(0, 0, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kAlreadySet);
    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kBusy);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.instance == 1 && sink.length == 5 &&
           sink.report[0] == 0 && sink.report[1] == 1);
    assert(state.mouse_report_snapshot().state ==
           hid_runtime::MouseReportTicketState::kSubmitted);
    state.report_complete(1);
    state.finalize_mouse_report();

    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 1 && sink.report[1] == 1);
    state.report_complete(1);
    state.finalize_mouse_report();
    assert(state.mouse_state().buttons == 0);

    // Button state is provisional until completion; relative axes never
    // become persistent logical state.
    assert(state.begin_mouse_report(3, 1, -2, 4, -5) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.report[0] == 3 && sink.report[1] == 1 &&
           sink.report[2] == static_cast<std::uint8_t>(-2) && sink.report[3] == 4 &&
           sink.report[4] == static_cast<std::uint8_t>(-5));
    assert(state.mouse_state().buttons == 3);
    state.report_complete(1);
    state.finalize_mouse_report();
    assert(state.mouse_state().buttons == 3);
    assert(state.begin_mouse_report(3, 0, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kAlreadySet);
}

void test_mouse_report_ticket_cancellation_and_failure() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    assert(state.cancel_mouse_report(state.mouse_report_snapshot().ticket_id));
    state.execute(Sink::submit, &sink);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    state.finalize_mouse_report();

    // Lifecycle invalidation cancels published work and prevents later replay.
    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    const auto epoch = state.authority_epoch();
    state.on_suspend();
    assert(state.authority_epoch() != epoch);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.mouse_report_snapshot().outcome ==
           hid_runtime::MouseReportTicketOutcome::kAuthorityLost);
    state.finalize_mouse_report();
    state.on_resume();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);

    // TinyUSB submit=false is terminal for the unsafe report and does not
    // cause a later SOF retry or an inverse movement.
    sink.accept = false;
    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 &&
           state.mouse_report_snapshot().state ==
               hid_runtime::MouseReportTicketState::kNotReady);
    state.finalize_mouse_report();
    sink.accept = true;
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);

    // A failed relative-only report enters the existing safety barrier; only
    // the zero all-up safety report may follow.
    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2);
    assert(state.report_failed(1));
    assert(state.safety_required(hid_runtime::Interface::kMouse));
    state.finalize_mouse_report();
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[0] == 0 &&
           sink.report[1] == 0);
    state.report_complete(1);
}

void test_mouse_release_all_during_in_flight() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && state.report_in_flight(hid_runtime::Interface::kMouse));
    state.request_release_all();
    state.report_complete(1);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 1 && sink.report[0] == 0 &&
           sink.report[1] == 0 && sink.report[2] == 0 && sink.report[3] == 0 &&
           sink.report[4] == 0);
    state.report_complete(1);
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
}

void invalidate_at_ticket_publish(hid_runtime::StateMachine *state) {
    state->on_unmount();
}

void invalidate_before_submit(hid_runtime::StateMachine *state) {
    state->on_unmount();
}

struct FakeLifecycleExecutor final : usb_lifecycle::Executor {
    bool schedule(usb_lifecycle::ExecutorAction action,
                  usb_lifecycle::Snapshot snapshot) override {
        calls += 1;
        last_action = action;
        last_snapshot = snapshot;
        return true;
    }

    int calls = 0;
    usb_lifecycle::ExecutorAction last_action = usb_lifecycle::ExecutorAction::kInstall;
    usb_lifecycle::Snapshot last_snapshot{};
};

void test_writing_and_claimed_lifecycle_races_are_fail_closed() {
    const std::array<std::uint8_t, 6> keys = {4, 0, 0, 0, 0, 0};
    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        state.set_before_ticket_publish_hook_for_test(invalidate_at_ticket_publish);
        assert(state.begin_keyboard_report(0, keys) ==
               hid_runtime::KeyboardReportBeginResult::kAuthorityLost);
        state.set_before_ticket_publish_hook_for_test(nullptr);
        state.execute(Sink::submit, &sink);
        assert(sink.calls == 0);
    }
    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        assert(state.queue_mouse_report(0, 1, 0, 0, 0));
        state.set_before_submit_hook_for_test(invalidate_before_submit);
        state.execute(Sink::submit, &sink);
        state.set_before_submit_hook_for_test(nullptr);
        assert(sink.calls == 0);
    }
}

void test_late_tokenized_callbacks_cannot_affect_new_generation() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    const hid_runtime::HidWorkToken old_token =
        state.in_flight_token(hid_runtime::Interface::kMouse);
    state.on_unmount();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    // The prior in-flight report is unproven. A new attachment reconciles
    // only with fresh all-up work before it admits another unsafe ticket.
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0);
    state.report_complete(0);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[0] == 0);
    state.report_complete(1);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);

    assert(state.begin_mouse_report(0, 1, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    const hid_runtime::HidWorkToken new_token =
        state.in_flight_token(hid_runtime::Interface::kMouse);
    assert(old_token.transport_generation != new_token.transport_generation);
    assert(old_token.route_generation != new_token.route_generation);
    assert(new_token.transport == hid_runtime::HidTransport::kUsb);
    const hid_runtime::HidWorkToken stale_route = {
        .authority_epoch = new_token.authority_epoch,
        .route_generation = new_token.route_generation + 1,
        .transport = new_token.transport,
        .transport_generation = new_token.transport_generation,
        .ticket_id = new_token.ticket_id,
        .release_epoch = new_token.release_epoch,
    };
    assert(!state.report_complete_for_token(1, stale_route));
    assert(!state.report_complete_for_token(1, old_token));
    assert(!state.report_failed_for_token(1, old_token));
    assert(state.report_in_flight(hid_runtime::Interface::kMouse));
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
    assert(state.report_complete_for_token(1, new_token));
}

void test_relative_and_safety_work_do_not_cross_usb_generation() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    // A queued relative delta cannot become a post-reattach movement.
    assert(state.queue_mouse_report(0, 7, 0, 0, 0));
    state.on_unmount();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);

    // An old all-up retry is canceled rather than applied to the fresh
    // attachment. The retained uncertainty instead causes distinct
    // fresh-generation all-up reconciliation work.
    assert(state.queue_mouse_report(1, 0, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    state.report_complete(1);
    state.request_release_all();
    state.on_unmount();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0);
    state.report_complete(0);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[0] == 0);
    state.report_complete(1);
}

void test_internal_detach_preserves_uncertainty_until_explicit_recovery() {
    hid_runtime::StateMachine state;
    Sink sink;
    FakeLifecycleExecutor executor;
    ready(state);
    assert(state.queue_mouse_report(1, 0, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(state.report_in_flight(hid_runtime::Interface::kMouse));

    state.request_usb_detach(executor);
    const auto detached = state.usb_lifecycle_snapshot();
    assert(executor.calls == 1);
    assert(detached.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(detached.observed == usb_lifecycle::ObservedState::kDetaching);
    assert(detached.safety_pending);
    assert(!detached.host_release_uncertain);
    const auto old_generation = detached.generation;
    assert(!state.queue_mouse_report(0, 1, 0, 0, 0));
    assert(action(state.request_usb_attach(executor)) == usb_lifecycle::TransitionResult::kBusy);

    // The in-flight old-generation report needs lifecycle-owned all-up. A
    // failed/timed-out resolution preserves uncertainty before teardown.
    assert(state.begin_lifecycle_detach_safety() == hid_runtime::LifecycleSafetyResult::kPending);
    state.mark_lifecycle_detach_uncertain(old_generation);
    assert(state.begin_usb_uninstall() == old_generation + 1);
    state.on_driver_uninstalled();
    state.complete_usb_uninstall_success();
    assert(state.usb_lifecycle_snapshot().host_release_uncertain);

    // A fresh stack may be installed, but cannot admit unsafe work until it
    // sends fresh-generation safety reports.
    assert(action(state.request_usb_attach(executor)) == usb_lifecycle::TransitionResult::kAccepted);
    state.complete_usb_install_success();
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(!state.queue_mouse_report(0, 1, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.instance == 0 && sink.report[0] == 0);
    state.report_complete(0);
    state.execute(Sink::submit, &sink);
    assert(sink.instance == 1 && sink.report[0] == 0);
    state.report_complete(1);
    assert(state.queue_mouse_report(0, 1, 0, 0, 0));

    // A clean lifecycle can, after a fresh generation and fresh authority,
    // accept new work normally.
    hid_runtime::StateMachine clean;
    ready(clean);
    const auto clean_old_generation = clean.attach_generation();
    assert(action(clean.request_usb_detach(executor)) == usb_lifecycle::TransitionResult::kAccepted);
    assert(clean.begin_lifecycle_detach_safety() == hid_runtime::LifecycleSafetyResult::kClean);
    assert(clean.begin_usb_uninstall() == clean_old_generation + 1);
    clean.on_driver_uninstalled();
    clean.complete_usb_uninstall_success();
    assert(action(clean.request_usb_attach(executor)) == usb_lifecycle::TransitionResult::kAccepted);
    clean.complete_usb_install_success();
    clean.on_mount();
    clean.set_ready(hid_runtime::Interface::kKeyboard, true);
    clean.set_ready(hid_runtime::Interface::kMouse, true);
    assert(clean.queue_mouse_report(0, 1, 0, 0, 0));
}

void test_hidden_uncertainty_is_never_zero_work_terminalized() {
    hid_runtime::StateMachine state;
    Sink sink;
    FakeLifecycleExecutor executor;
    ready(state);
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, f24));
    state.execute(Sink::submit, &sink);
    assert(state.report_complete(0));
    state.on_unmount();
    assert(state.usb_lifecycle_snapshot().host_release_uncertain);

    assert(action(state.request_usb_detach(executor)) ==
           usb_lifecycle::TransitionResult::kAccepted);
    const auto old_generation = state.attach_generation();
    state.mark_lifecycle_detach_uncertain(old_generation);
    assert(state.begin_usb_uninstall() == old_generation + 1);
    state.on_driver_uninstalled();
    state.complete_usb_uninstall_success();
    const auto hidden_uncertain = state.usb_lifecycle_snapshot();
    assert(hidden_uncertain.desired == usb_lifecycle::DesiredExposure::kHidden);
    assert(hidden_uncertain.observed ==
           usb_lifecycle::ObservedState::kDriverNotInstalled);
    assert(hidden_uncertain.host_release_uncertain);

    state.request_release_all();
    const auto pending = state.usb_lifecycle_snapshot();
    assert(pending.safety_pending);
    assert(pending.host_release_uncertain);
    assert(state.release_requested_for_test());
    assert(sink.calls == 1);
}

void test_detach_invalidates_route_only_after_old_route_safety() {
    hid_runtime::StateMachine state;
    Sink sink;
    FakeLifecycleExecutor executor;
    ready(state);
    const hid_route::Snapshot old_route = state.route_snapshot();
    const auto old_usb_generation = state.attach_generation();
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, f24));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.instance == 0 && sink.report[2] == 115);

    assert(action(state.request_usb_detach(executor)) == usb_lifecycle::TransitionResult::kAccepted);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kUsb);
    assert(state.begin_lifecycle_detach_safety() == hid_runtime::LifecycleSafetyResult::kPending);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0 && sink.report[2] == 0);
    assert(state.report_complete(0));
    assert(state.lifecycle_detach_safety_clean());

    state.complete_usb_detach_route_invalidation(old_route);
    const hid_route::Snapshot invalidated = state.route_snapshot();
    assert(invalidated.active == hid_route::OutputRoute::kNone);
    assert(invalidated.generation == old_route.generation + 1);
    assert(state.begin_usb_uninstall() == old_usb_generation + 1);
}

void test_route_zero_work_round_trip_is_coherent_and_keeps_usb_exposed() {
    hid_runtime::StateMachine state;
    ready(state);
    const auto usb_before = state.route_status_snapshot();
    assert(usb_before.ready);
    assert(usb_before.route.active == hid_route::OutputRoute::kUsb);
    const auto authority_before = state.authority_epoch();

    const auto release = state.request_route_none();
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(release.snapshot_valid && !release.async_required);
    assert(release.snapshot.route.desired == hid_route::OutputRoute::kNone);
    assert(release.snapshot.route.active == hid_route::OutputRoute::kUsb);
    assert(release.snapshot.route.transition == hid_route::Transition::kReleasing);
    assert(!release.snapshot.ready);
    assert(state.authority_epoch() == authority_before + 1);

    const auto none = state.route_status_snapshot();
    assert(none.route.desired == hid_route::OutputRoute::kNone);
    assert(none.route.active == hid_route::OutputRoute::kNone);
    assert(none.route.transition == hid_route::Transition::kStable);
    assert(none.route.generation == release.snapshot.route.generation + 1);
    assert(!none.ready);
    const auto lifecycle = state.usb_lifecycle_snapshot();
    assert(lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed);
    assert(lifecycle.observed == usb_lifecycle::ObservedState::kMounted);

    const auto none_noop = state.request_route_none();
    assert(none_noop.action_result == hid_runtime::RouteTransitionResult::kNoOp);
    assert(none_noop.snapshot.route.generation == none.route.generation);

    const auto select = state.request_route_usb();
    assert(select.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(select.snapshot.route.active == hid_route::OutputRoute::kUsb);
    assert(select.snapshot.route.generation == none.route.generation + 1);
    assert(select.snapshot.ready);
    const auto usb_noop = state.request_route_usb();
    assert(usb_noop.action_result == hid_runtime::RouteTransitionResult::kNoOp);
    assert(usb_noop.snapshot.route.generation == select.snapshot.route.generation);
}

void test_route_held_key_release_uses_old_route_before_none_commit() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, f24));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1 && sink.report[2] == 115);
    assert(state.report_complete(0));

    const auto release = state.request_route_none();
    assert(release.action_result == hid_runtime::RouteTransitionResult::kAccepted);
    assert(release.async_required);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kUsb);
    assert(state.begin_route_release_safety(release.snapshot.route) ==
           hid_runtime::LifecycleSafetyResult::kPending);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0);
    assert(sink.report[0] == 0 && sink.report[2] == 0);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kUsb);
    assert(state.report_complete(0));
    assert(state.lifecycle_detach_safety_clean());
    state.complete_route_release(release.snapshot.route);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.usb_lifecycle_snapshot().observed ==
           usb_lifecycle::ObservedState::kMounted);
}

void test_route_release_callback_preemption_fails_closed() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    const std::array<std::uint8_t, 6> f24 = {115, 0, 0, 0, 0, 0};
    assert(state.queue_keyboard_report(0, f24));
    state.execute(Sink::submit, &sink);
    assert(state.report_complete(0));
    const auto release = state.request_route_none();
    assert(release.async_required);

    state.on_suspend();
    const auto none = state.route_snapshot();
    assert(none.active == hid_route::OutputRoute::kNone);
    assert(none.transition == hid_route::Transition::kStable);
    assert(!state.queue_keyboard_report(0, f24));
    assert(state.begin_route_release_safety(release.snapshot.route) ==
           hid_runtime::LifecycleSafetyResult::kUncertain);
}

struct ReleaseBoundarySink {
    hid_runtime::StateMachine *state = nullptr;
    bool release_at_adapter_entry = false;
    bool mouse = false;
    int calls = 0;
    std::array<std::uint8_t, 8> first{};
    std::array<std::uint8_t, 8> last{};
    std::uint16_t last_length = 0;

    static bool submit(void *context, std::uint8_t,
                       const std::uint8_t *report, std::uint16_t length) {
        auto *sink = static_cast<ReleaseBoundarySink *>(context);
        if (sink->release_at_adapter_entry && sink->calls == 0) {
            sink->state->begin_release_all();
            const auto release = sink->state->release_all_snapshot();
            assert((sink->mouse ? release.mouse : release.keyboard) !=
                   hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
        }
        ++sink->calls;
        sink->last_length = length;
        std::memcpy(sink->last.data(), report, length);
        if (sink->calls == 1) sink->first = sink->last;
        return true;
    }
};

void release_from_runtime_hook(hid_runtime::StateMachine *state) {
    state->begin_release_all();
    assert(state->release_all_snapshot().keyboard !=
           hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
}

void finish_release_after_keyboard_boundary(hid_runtime::StateMachine &state,
                                            ReleaseBoundarySink &sink) {
    const auto ticket = state.keyboard_report_snapshot();
    if (ticket.state == hid_runtime::KeyboardReportTicketState::kSubmitted) {
        assert(state.report_complete(0, sink.last.data(), sink.last_length));
    }
    state.set_before_submit_hook_for_test(nullptr);
    state.set_after_submit_hook_for_test(nullptr);
    state.execute(ReleaseBoundarySink::submit, &sink);
    assert(sink.calls >= 1);
    assert(sink.last_length == 8 && sink.last[0] == 0 && sink.last[2] == 0);
    assert(state.report_complete(0, sink.last.data(), sink.last_length));
    assert(state.release_all_snapshot().keyboard ==
           hid_runtime::ReleaseAllInterfaceState::kPending);
    state.finalize_release_all();
    state.execute(ReleaseBoundarySink::submit, &sink);
    state.begin_release_all();
    assert(state.release_all_snapshot().keyboard ==
           hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    state.finalize_release_all();
}

void test_release_serializes_claimed_and_submitting_tickets() {
    // Published but not claimed: the release epoch cancels the press before
    // its claim, so the public release may complete AlreadyUp.
    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId ticket = 0;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {},
                                           &ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        state.begin_release_all();
        ReleaseBoundarySink sink{.state = &state};
        assert(state.release_all_snapshot().keyboard ==
               hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
        state.finalize_release_all();
        state.execute(ReleaseBoundarySink::submit, &sink);
        assert(sink.calls == 0);
        hid_runtime::KeyboardReportSnapshot canceled{};
        assert(state.keyboard_report_snapshot(ticket, &canceled));
        assert(canceled.state ==
               hid_runtime::KeyboardReportTicketState::kCanceled);
        assert(state.finalize_keyboard_report(ticket));
    }

    // Claimed before the final fence: revocation prevents the press.
    {
        hid_runtime::StateMachine state;
        ready(state);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        state.set_before_submit_hook_for_test(release_from_runtime_hook);
        ReleaseBoundarySink sink{.state = &state};
        state.execute(ReleaseBoundarySink::submit, &sink);
        finish_release_after_keyboard_boundary(state, sink);
        assert(sink.first[2] == 0);
    }

    // Adapter entry is the USB submission linearization point. Release sees
    // the claimed ticket, stays pending, and safety submits after the press.
    {
        hid_runtime::StateMachine state;
        ready(state);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        ReleaseBoundarySink sink{.state = &state,
                                 .release_at_adapter_entry = true};
        state.execute(ReleaseBoundarySink::submit, &sink);
        assert(sink.calls == 1 && sink.first[2] == 4);
        finish_release_after_keyboard_boundary(state, sink);
        assert(sink.calls == 2 && sink.last[2] == 0);
    }

    // Accepted by the adapter but not yet published in-flight is covered by
    // the same claimed-ticket release barrier.
    {
        hid_runtime::StateMachine state;
        ready(state);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        state.set_after_submit_hook_for_test(release_from_runtime_hook);
        ReleaseBoundarySink sink{.state = &state};
        state.execute(ReleaseBoundarySink::submit, &sink);
        assert(sink.calls == 1 && sink.first[2] == 4);
        finish_release_after_keyboard_boundary(state, sink);
        assert(sink.calls == 2 && sink.last[2] == 0);
    }

    // Once in-flight is visible, release likewise serializes all-up after it.
    {
        hid_runtime::StateMachine state;
        ready(state);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        ReleaseBoundarySink sink{.state = &state};
        state.execute(ReleaseBoundarySink::submit, &sink);
        state.begin_release_all();
        assert(state.release_all_snapshot().keyboard !=
               hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
        finish_release_after_keyboard_boundary(state, sink);
        assert(sink.calls == 2 && sink.first[2] == 4 && sink.last[2] == 0);
    }

    // Mouse tickets share the claim/submission barrier.
    {
        hid_runtime::StateMachine state;
        ready(state);
        assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        ReleaseBoundarySink sink{.state = &state,
                                 .release_at_adapter_entry = true,
                                 .mouse = true};
        state.execute(ReleaseBoundarySink::submit, &sink);
        assert(sink.calls == 1 && sink.first[0] == 1);
        assert(state.report_complete(1, sink.last.data(), sink.last_length));
        state.execute(ReleaseBoundarySink::submit, &sink);
        assert(sink.calls == 2 && sink.last_length == 5 && sink.last[0] == 0);
        assert(state.report_complete(1, sink.last.data(), sink.last_length));
        assert(state.release_all_snapshot().mouse ==
               hid_runtime::ReleaseAllInterfaceState::kPending);
        state.finalize_release_all();
        state.execute(ReleaseBoundarySink::submit, &sink);
        state.begin_release_all();
        assert(state.release_all_snapshot().mouse ==
               hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    }
}

void test_writing_cancellation_keeps_slot_writer_owned_until_acknowledged() {
    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        writing_probe_interface = WritingProbeInterface::kKeyboard;
        writing_probe_ticket = 0;
        state.set_before_ticket_publish_hook_for_test(
            cancel_writer_owned_ticket);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kSafetyPending);
        assert(writing_probe_ticket != 0);
        assert(writing_probe_keyboard_result ==
               hid_runtime::KeyboardReportBeginResult::kBusy);
        hid_runtime::KeyboardReportSnapshot old_snapshot{};
        assert(state.keyboard_report_snapshot(writing_probe_ticket,
                                              &old_snapshot));
        assert(old_snapshot.state ==
               hid_runtime::KeyboardReportTicketState::kCanceled);
        assert(old_snapshot.outcome ==
               hid_runtime::KeyboardReportTicketOutcome::kSafetyPending);
        state.execute(Sink::submit, &sink);
        assert(sink.calls == 0);
        state.finalize_release_all();
        assert(state.finalize_keyboard_report(writing_probe_ticket));

        hid_runtime::HidTicketId replacement = 0;
        assert(state.begin_keyboard_report(0, {5, 0, 0, 0, 0, 0}, {},
                                           &replacement) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(replacement != writing_probe_ticket);
        state.execute(Sink::submit, &sink);
        assert(sink.calls == 1 && sink.report[2] == 5);
        assert(state.report_complete(0, sink.report.data(), sink.length));
        assert(state.finalize_keyboard_report(replacement));
    }

    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        writing_probe_interface = WritingProbeInterface::kMouse;
        writing_probe_ticket = 0;
        state.set_before_ticket_publish_hook_for_test(
            cancel_writer_owned_ticket);
        assert(state.begin_mouse_report(1, 1, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kSafetyPending);
        assert(writing_probe_ticket != 0);
        assert(writing_probe_mouse_result ==
               hid_runtime::MouseReportBeginResult::kBusy);
        hid_runtime::MouseReportSnapshot old_snapshot{};
        assert(state.mouse_report_snapshot(writing_probe_ticket, &old_snapshot));
        assert(old_snapshot.state ==
               hid_runtime::MouseReportTicketState::kCanceled);
        assert(old_snapshot.outcome ==
               hid_runtime::MouseReportTicketOutcome::kSafetyPending);
        state.execute(Sink::submit, &sink);
        assert(sink.calls == 0);
        state.finalize_release_all();
        assert(state.finalize_mouse_report(writing_probe_ticket));

        hid_runtime::HidTicketId replacement = 0;
        assert(state.begin_mouse_report(2, 3, 0, 0, 0, {}, &replacement) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(replacement != writing_probe_ticket);
        state.execute(Sink::submit, &sink);
        assert(sink.calls == 1 && sink.report[0] == 2 && sink.report[1] == 3);
        assert(state.report_complete(1, sink.report.data(), sink.length));
        assert(state.finalize_mouse_report(replacement));
    }
}

void test_exact_cancel_transition_excludes_reuse() {
    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {},
                                           &old_ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        reset_transition_probe();
        state.set_inside_ticket_cancel_hook_for_test(
            hold_exact_ticket_transition);
        bool canceled = false;
        std::thread old_caller(
            [&] { canceled = state.cancel_keyboard_report(old_ticket); });
        while (!transition_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        hid_runtime::HidTicketId replacement = 0;
        hid_runtime::KeyboardReportBeginResult replacement_result{};
        std::thread replacement_caller([&] {
            transition_contender_started.store(true, std::memory_order_release);
            replacement_result = state.begin_keyboard_report(
                0, {5, 0, 0, 0, 0, 0}, {}, &replacement);
            transition_contender_finished.store(true, std::memory_order_release);
        });
        old_caller.join();
        replacement_caller.join();
        state.set_inside_ticket_cancel_hook_for_test(nullptr);
        assert(canceled);
        assert(replacement_result ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(replacement != 0 && replacement != old_ticket);
        assert(!state.cancel_keyboard_report(old_ticket));
        assert(!state.finalize_keyboard_report(old_ticket));
        hid_runtime::KeyboardReportSnapshot snapshot{};
        assert(state.keyboard_report_snapshot(replacement, &snapshot));
        assert(snapshot.state ==
               hid_runtime::KeyboardReportTicketState::kPublished);
        assert(state.cancel_keyboard_report(replacement));
        assert(state.finalize_keyboard_report(replacement));
    }

    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_mouse_report(1, 1, 0, 0, 0, {}, &old_ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        reset_transition_probe();
        state.set_inside_ticket_cancel_hook_for_test(
            hold_exact_ticket_transition);
        bool canceled = false;
        std::thread old_caller(
            [&] { canceled = state.cancel_mouse_report(old_ticket); });
        while (!transition_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        hid_runtime::HidTicketId replacement = 0;
        hid_runtime::MouseReportBeginResult replacement_result{};
        std::thread replacement_caller([&] {
            transition_contender_started.store(true, std::memory_order_release);
            replacement_result =
                state.begin_mouse_report(2, 2, 0, 0, 0, {}, &replacement);
            transition_contender_finished.store(true, std::memory_order_release);
        });
        old_caller.join();
        replacement_caller.join();
        state.set_inside_ticket_cancel_hook_for_test(nullptr);
        assert(canceled);
        assert(replacement_result ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(replacement != 0 && replacement != old_ticket);
        assert(!state.cancel_mouse_report(old_ticket));
        assert(!state.finalize_mouse_report(old_ticket));
        hid_runtime::MouseReportSnapshot snapshot{};
        assert(state.mouse_report_snapshot(replacement, &snapshot));
        assert(snapshot.state == hid_runtime::MouseReportTicketState::kPublished);
        assert(state.cancel_mouse_report(replacement));
        assert(state.finalize_mouse_report(replacement));
    }
}

void test_exact_finalize_transition_excludes_reuse() {
    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {},
                                           &old_ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(state.cancel_keyboard_report(old_ticket));
        reset_transition_probe();
        state.set_inside_ticket_finalize_hook_for_test(
            hold_exact_ticket_transition);
        bool finalized = false;
        std::thread old_caller(
            [&] { finalized = state.finalize_keyboard_report(old_ticket); });
        while (!transition_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        hid_runtime::HidTicketId replacement = 0;
        hid_runtime::KeyboardReportBeginResult replacement_result{};
        std::thread replacement_caller([&] {
            transition_contender_started.store(true, std::memory_order_release);
            replacement_result = state.begin_keyboard_report(
                0, {5, 0, 0, 0, 0, 0}, {}, &replacement);
            transition_contender_finished.store(true, std::memory_order_release);
        });
        old_caller.join();
        replacement_caller.join();
        state.set_inside_ticket_finalize_hook_for_test(nullptr);
        assert(finalized);
        assert(replacement_result ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(replacement != old_ticket);
        assert(!state.finalize_keyboard_report(old_ticket));
        assert(state.cancel_keyboard_report(replacement));
        assert(state.finalize_keyboard_report(replacement));
    }

    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_mouse_report(1, 1, 0, 0, 0, {}, &old_ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(state.cancel_mouse_report(old_ticket));
        reset_transition_probe();
        state.set_inside_ticket_finalize_hook_for_test(
            hold_exact_ticket_transition);
        bool finalized = false;
        std::thread old_caller(
            [&] { finalized = state.finalize_mouse_report(old_ticket); });
        while (!transition_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        hid_runtime::HidTicketId replacement = 0;
        hid_runtime::MouseReportBeginResult replacement_result{};
        std::thread replacement_caller([&] {
            transition_contender_started.store(true, std::memory_order_release);
            replacement_result =
                state.begin_mouse_report(2, 2, 0, 0, 0, {}, &replacement);
            transition_contender_finished.store(true, std::memory_order_release);
        });
        old_caller.join();
        replacement_caller.join();
        state.set_inside_ticket_finalize_hook_for_test(nullptr);
        assert(finalized);
        assert(replacement_result ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(replacement != old_ticket);
        assert(!state.finalize_mouse_report(old_ticket));
        assert(state.cancel_mouse_report(replacement));
        assert(state.finalize_mouse_report(replacement));
    }
}

void test_terminal_state_never_precedes_terminal_outcome() {
    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId ticket = 0;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {},
                                           &ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        reset_terminal_probe();
        state.set_before_terminal_ticket_publish_hook_for_test(
            hold_before_terminal_publication);
        bool canceled = false;
        std::thread producer(
            [&] { canceled = state.cancel_keyboard_report(ticket); });
        while (!terminal_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        bool found = false;
        hid_runtime::KeyboardReportSnapshot observed{};
        std::thread observer([&] {
            terminal_observer_started.store(true, std::memory_order_release);
            found = state.keyboard_report_snapshot(ticket, &observed);
            terminal_observer_finished.store(true, std::memory_order_release);
        });
        producer.join();
        observer.join();
        state.set_before_terminal_ticket_publish_hook_for_test(nullptr);
        assert(canceled && found);
        assert(observed.state ==
               hid_runtime::KeyboardReportTicketState::kCanceled);
        assert(observed.outcome ==
               hid_runtime::KeyboardReportTicketOutcome::kNotReady);
        assert(state.finalize_keyboard_report(ticket));
    }

    {
        hid_runtime::StateMachine state;
        ready(state);
        hid_runtime::HidTicketId ticket = 0;
        assert(state.begin_mouse_report(1, 1, 0, 0, 0, {}, &ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        reset_terminal_probe();
        state.set_before_terminal_ticket_publish_hook_for_test(
            hold_before_terminal_publication);
        bool canceled = false;
        std::thread producer(
            [&] { canceled = state.cancel_mouse_report(ticket); });
        while (!terminal_hook_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        bool found = false;
        hid_runtime::MouseReportSnapshot observed{};
        std::thread observer([&] {
            terminal_observer_started.store(true, std::memory_order_release);
            found = state.mouse_report_snapshot(ticket, &observed);
            terminal_observer_finished.store(true, std::memory_order_release);
        });
        producer.join();
        observer.join();
        state.set_before_terminal_ticket_publish_hook_for_test(nullptr);
        assert(canceled && found);
        assert(observed.state == hid_runtime::MouseReportTicketState::kCanceled);
        assert(observed.outcome ==
               hid_runtime::MouseReportTicketOutcome::kNotReady);
        assert(state.finalize_mouse_report(ticket));
    }
}

void test_public_ticket_identity_width_boundary_and_exhaustion() {
    static_assert(sizeof(hid_runtime::HidTicketId) >= 8);
    hid_runtime::StateMachine state;
    ready(state);

    state.set_next_public_ticket_id_for_test(
        std::numeric_limits<std::uint32_t>::max());
    hid_runtime::HidTicketId low = 0;
    assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {}, &low) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(low == std::numeric_limits<std::uint32_t>::max());
    assert(state.cancel_keyboard_report(low));
    assert(state.finalize_keyboard_report(low));

    hid_runtime::HidTicketId high = 0;
    assert(state.begin_mouse_report(1, 1, 0, 0, 0, {}, &high) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    assert(high ==
           static_cast<hid_runtime::HidTicketId>(
               std::numeric_limits<std::uint32_t>::max()) +
               1);
    assert(high != low);
    hid_runtime::MouseReportSnapshot high_snapshot{};
    assert(state.mouse_report_snapshot(high, &high_snapshot));
    assert(!state.mouse_report_snapshot(low, &high_snapshot));
    assert(state.cancel_mouse_report(high));
    assert(state.finalize_mouse_report(high));

    state.set_next_public_ticket_id_for_test(
        std::numeric_limits<hid_runtime::HidTicketId>::max());
    hid_runtime::HidTicketId final_ticket = 0;
    assert(state.begin_keyboard_report(0, {6, 0, 0, 0, 0, 0}, {},
                                       &final_ticket) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(final_ticket ==
           std::numeric_limits<hid_runtime::HidTicketId>::max());
    assert(state.cancel_keyboard_report(final_ticket));
    assert(state.finalize_keyboard_report(final_ticket));

    hid_runtime::HidTicketId exhausted_ticket = 123;
    assert(state.begin_mouse_report(1, 2, 0, 0, 0, {}, &exhausted_ticket) ==
           hid_runtime::MouseReportBeginResult::kNotReady);
    assert(exhausted_ticket == 0);
    assert(state.mouse_report_snapshot().state ==
           hid_runtime::MouseReportTicketState::kFree);
}

void test_ticket_identity_prevents_result_theft_and_wrong_free() {
    for (const bool old_waiter_first : {false, true}) {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}, {},
                                           &old_ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(state.cancel_keyboard_report(old_ticket));
        hid_runtime::KeyboardReportSnapshot old_snapshot{};
        assert(state.keyboard_report_snapshot(old_ticket, &old_snapshot));
        assert(old_snapshot.state ==
               hid_runtime::KeyboardReportTicketState::kCanceled);

        if (old_waiter_first) {
            assert(state.finalize_keyboard_report(old_ticket));
        }
        hid_runtime::HidTicketId new_ticket = 0;
        assert(state.begin_keyboard_report(0, {5, 0, 0, 0, 0, 0}, {},
                                           &new_ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(new_ticket != old_ticket);
        if (!old_waiter_first) {
            assert(!state.keyboard_report_snapshot(old_ticket, &old_snapshot));
            assert(!state.finalize_keyboard_report(old_ticket));
        }
        state.execute(Sink::submit, &sink);
        hid_runtime::KeyboardReportSnapshot new_snapshot{};
        assert(state.keyboard_report_snapshot(new_ticket, &new_snapshot));
        assert(new_snapshot.state ==
               hid_runtime::KeyboardReportTicketState::kSubmitted);
        assert(state.finalize_keyboard_report(new_ticket));
    }

    for (const bool old_waiter_first : {false, true}) {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        hid_runtime::HidTicketId old_ticket = 0;
        assert(state.begin_mouse_report(1, 0, 0, 0, 0, {}, &old_ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(state.cancel_mouse_report(old_ticket));
        hid_runtime::MouseReportSnapshot old_snapshot{};
        assert(state.mouse_report_snapshot(old_ticket, &old_snapshot));
        assert(old_snapshot.state ==
               hid_runtime::MouseReportTicketState::kCanceled);
        if (old_waiter_first) {
            assert(state.finalize_mouse_report(old_ticket));
        }
        hid_runtime::HidTicketId new_ticket = 0;
        assert(state.begin_mouse_report(2, 0, 0, 0, 0, {}, &new_ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        assert(new_ticket != old_ticket);
        if (!old_waiter_first) {
            assert(!state.mouse_report_snapshot(old_ticket, &old_snapshot));
            assert(!state.finalize_mouse_report(old_ticket));
        }
        state.execute(Sink::submit, &sink);
        hid_runtime::MouseReportSnapshot new_snapshot{};
        assert(state.mouse_report_snapshot(new_ticket, &new_snapshot));
        assert(new_snapshot.state ==
               hid_runtime::MouseReportTicketState::kSubmitted);
        assert(state.finalize_mouse_report(new_ticket));
    }
}

void test_report_origin_owner_is_bound_to_exact_async_work() {
    constexpr hid_runtime::ReportOriginOwnerId high_owner =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
        17U;

    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        hid_runtime::HidTicketId ticket = 0;
        assert(state.begin_keyboard_report(
                   0, {4, 0, 0, 0, 0, 0}, {}, &ticket, high_owner) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        const auto published =
            state.published_report_token(hid_runtime::Interface::kKeyboard);
        assert(published.ticket_id == ticket);
        assert(published.originating_local_owner_id == high_owner);
        state.execute(Sink::submit, &sink);
        const auto in_flight =
            state.in_flight_token(hid_runtime::Interface::kKeyboard);
        assert(in_flight.originating_local_owner_id == high_owner);

        auto forged = in_flight;
        forged.originating_local_owner_id = high_owner + 1U;
        hid_runtime::ReportOriginOwnerId recovered = high_owner;
        assert(!state.report_failed_for_token(
            static_cast<std::uint8_t>(hid_runtime::Interface::kKeyboard),
            forged, nullptr, 0, &recovered));
        assert(recovered == 0);
        assert(state.report_in_flight(hid_runtime::Interface::kKeyboard));

        // The public operation remains attributable after an intervening
        // release request, which models a local-owner takeover barrier.
        state.request_release_all();
        assert(state.report_failed_for_token(
            static_cast<std::uint8_t>(hid_runtime::Interface::kKeyboard),
            in_flight, nullptr, 0, &recovered));
        assert(recovered == high_owner);
    }

    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        hid_runtime::HidTicketId ticket = 0;
        assert(state.begin_mouse_report(1, 3, -2, 1, -1, {}, &ticket,
                                        high_owner) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        const auto published =
            state.published_report_token(hid_runtime::Interface::kMouse);
        assert(published.ticket_id == ticket);
        assert(published.originating_local_owner_id == high_owner);
        state.execute(Sink::submit, &sink);
        const auto in_flight =
            state.in_flight_token(hid_runtime::Interface::kMouse);
        assert(in_flight.originating_local_owner_id == high_owner);
        hid_runtime::ReportOriginOwnerId recovered = 0;
        state.request_release_all();
        assert(state.report_failed_for_token(
            static_cast<std::uint8_t>(hid_runtime::Interface::kMouse),
            in_flight, nullptr, 0, &recovered));
        assert(recovered == high_owner);
    }

    {
        hid_runtime::StateMachine state;
        Sink sink;
        ready(state);
        assert(state.queue_keyboard_report(0, {4, 0, 0, 0, 0, 0}));
        state.execute(Sink::submit, &sink);
        const auto internal =
            state.in_flight_token(hid_runtime::Interface::kKeyboard);
        assert(internal.originating_local_owner_id == 0);
        hid_runtime::ReportOriginOwnerId recovered = high_owner;
        assert(state.report_failed_for_token(
            static_cast<std::uint8_t>(hid_runtime::Interface::kKeyboard),
            internal, nullptr, 0, &recovered));
        assert(recovered == 0);
    }
}

}  // namespace

int main() {
    test_lifecycle_and_generation_cancellation();
    test_readiness_refresh_after_mount_without_hid_work();
    test_sequence_admission_snapshot_and_producer_exclusion();
    test_revoked_sequence_authority_cannot_create_or_submit_ticket();
    test_sequence_ticket_paused_before_publication_cannot_survive_release();
    test_sequence_generation_does_not_wrap_to_stale_authority();
    test_ble_terminal_visibility_follows_confirmed_state();
    test_mailbox_sequence_exclusion_and_safety_priority();
    test_readiness_refresh_after_reattach();
    test_route_generation_is_independent_and_gates_stale_unsafe_work();
    test_usb_transition_outcomes_freeze_stage_a_runtime();
    test_success_failure_and_release();
    test_partial_release_and_no_delayed_unsafe_replay();
    test_zero_work_release_terminalizes_lifecycle_pending();
    test_hidden_clean_release_terminalizes_without_executor();
    test_disconnected_clean_release_terminalizes_without_executor();
    test_new_hidden_request_survives_old_reconciliation();
    test_attach_boundary_blocks_stale_hidden_reconciliation();
    test_later_attach_after_clean_hidden_release_has_no_stale_work();
    test_uncertainty_is_not_zero_work_terminalized();
    test_held_keyboard_release_requires_safety_completion();
    test_new_release_request_survives_zero_work_reconciliation_race();
    test_release_barrier_discards_queued_work();
    test_release_all_noop_for_known_all_up();
    test_executor_submission_bound();
    test_authority_epoch_suspend_resume_barrier();
    test_suspend_preserves_safety_and_ignores_late_completion();
    test_unmount_preserves_uncertainty_for_fresh_generation_reconciliation();
    test_release_ticket_states_and_historical_submission();
    test_release_ticket_failure_and_lifecycle_cancellation();
    test_release_ticket_partial_and_clean_unmounted();
    test_keyboard_report_ticket_and_confirmed_state();
    test_keyboard_report_ticket_cancellation_and_barriers();
    test_keyboard_report_submit_false_is_not_replayed();
    test_mouse_report_ticket_relative_and_confirmed_state();
    test_mouse_report_ticket_cancellation_and_failure();
    test_mouse_release_all_during_in_flight();
    test_writing_and_claimed_lifecycle_races_are_fail_closed();
    test_late_tokenized_callbacks_cannot_affect_new_generation();
    test_relative_and_safety_work_do_not_cross_usb_generation();
    test_internal_detach_preserves_uncertainty_until_explicit_recovery();
    test_hidden_uncertainty_is_never_zero_work_terminalized();
    test_detach_invalidates_route_only_after_old_route_safety();
    test_route_zero_work_round_trip_is_coherent_and_keeps_usb_exposed();
    test_route_held_key_release_uses_old_route_before_none_commit();
    test_route_release_callback_preemption_fails_closed();
    test_release_serializes_claimed_and_submitting_tickets();
    test_writing_cancellation_keeps_slot_writer_owned_until_acknowledged();
    test_exact_cancel_transition_excludes_reuse();
    test_exact_finalize_transition_excludes_reuse();
    test_terminal_state_never_precedes_terminal_outcome();
    test_public_ticket_identity_width_boundary_and_exhaustion();
    test_ticket_identity_prevents_result_theft_and_wrong_free();
    test_report_origin_owner_is_bound_to_exact_async_work();
    return 0;
}
