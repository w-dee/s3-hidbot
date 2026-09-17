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

enum class RoutePublicationInterleave {
    kSuspend,
    kSuspendResume,
    kUnmount,
    kUnmountRemount,
    kRuntimeFault,
};

hid_runtime::StateMachine *route_publication_runtime = nullptr;
RoutePublicationInterleave route_publication_interleave =
    RoutePublicationInterleave::kSuspend;

void inject_route_publication_lifecycle(hid_route::StateMachine &) {
    assert(route_publication_runtime != nullptr);
    switch (route_publication_interleave) {
        case RoutePublicationInterleave::kSuspend:
            route_publication_runtime->on_suspend();
            return;
        case RoutePublicationInterleave::kSuspendResume:
            route_publication_runtime->on_suspend();
            route_publication_runtime->on_resume();
            return;
        case RoutePublicationInterleave::kUnmount:
            route_publication_runtime->on_unmount();
            return;
        case RoutePublicationInterleave::kUnmountRemount:
            route_publication_runtime->on_unmount();
            route_publication_runtime->on_mount();
            return;
        case RoutePublicationInterleave::kRuntimeFault:
            assert(route_publication_runtime->begin_usb_runtime_fault(91));
            return;
    }
}

void inject_suspend_resume_before_route_registration(
    hid_runtime::StateMachine *state) {
    state->on_suspend();
    state->on_resume();
}

void prepare_route_publication(hid_runtime::StateMachine &state) {
    expose(state);
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
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

void make_ble_ready(
    hid_runtime::StateMachine &state,
    hid_runtime::ReportMask present_roles = hid_capability::kInputRoles) {
    const auto route = state.route_snapshot();
    hid_runtime::ReportHandles handles{};
    if (hid_capability::has_role(
            present_roles, hid_runtime::ReportRole::kKeyboardInput)) {
        handles.set(hid_runtime::ReportRole::kKeyboardInput, 13);
    }
    if (hid_capability::has_role(
            present_roles, hid_runtime::ReportRole::kMouseInput)) {
        handles.set(hid_runtime::ReportRole::kMouseInput, 14);
    }
    assert(state.request_route_ble({
        .expected_authority_epoch = state.authority_epoch(),
        .expected_route_generation = route.generation,
        .ble_generation = 11,
        .connection_handle = 12,
        .present_roles = present_roles,
        .required_input_subscriptions = present_roles,
        .report_handles = handles,
    }).action_result == hid_runtime::RouteTransitionResult::kAccepted);
}

void test_capability_aware_ble_routes_and_activation_epoch() {
    {
        hid_runtime::StateMachine state;
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kNotReady);
        assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kNotReady);

        ready(state);
        assert(state.begin_keyboard_report(0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(state.cancel_keyboard_report(
            state.keyboard_report_snapshot().ticket_id));
        state.finalize_keyboard_report();
        assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kPublished);
    }

    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kMouseInput);
        const auto authority = state.ble_route_authority_snapshot();
        assert(authority.profile_activation_epoch != 0);
        assert(authority.present_roles == hid_capability::kMouseInput);
        assert(authority.report_handles.get(
                   hid_runtime::ReportRole::kKeyboardInput) == 0);

        hid_runtime::HidTicketId ticket = 99;
        assert(state.begin_keyboard_report(
                   0, {4, 0, 0, 0, 0, 0}, {}, &ticket) ==
               hid_runtime::KeyboardReportBeginResult::kUnsupportedOperation);
        assert(ticket == 0);
        assert(state.keyboard_report_snapshot().state ==
               hid_runtime::KeyboardReportTicketState::kFree);
        assert(state.keyboard_state().keycodes[0] == 0);

        assert(state.begin_mouse_report(1, 0, 0, 0, 0, {}, &ticket) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        const auto work =
            state.published_report_token(hid_runtime::Interface::kMouse);
        assert(work.profile_activation_epoch ==
               authority.profile_activation_epoch);
        assert(state.mark_ble_report_scheduled(
            hid_runtime::Interface::kMouse, work));
        assert(state.process_ble_report(hid_runtime::Interface::kMouse, work,
                                        accept_ble_report, nullptr));
        state.finalize_mouse_report();
    }

    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kKeyboardInput);
        hid_runtime::HidTicketId ticket = 99;
        assert(state.begin_mouse_report(1, 0, 0, 0, 0, {}, &ticket) ==
               hid_runtime::MouseReportBeginResult::kUnsupportedOperation);
        assert(ticket == 0);
        assert(state.mouse_report_snapshot().state ==
               hid_runtime::MouseReportTicketState::kFree);
        assert(state.mouse_state().buttons == 0);

        assert(state.begin_keyboard_report(
                   0, {4, 0, 0, 0, 0, 0}, {}, &ticket) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        const auto work =
            state.published_report_token(hid_runtime::Interface::kKeyboard);
        assert(state.mark_ble_report_scheduled(
            hid_runtime::Interface::kKeyboard, work));
        assert(state.process_ble_report(hid_runtime::Interface::kKeyboard,
                                        work, accept_ble_report, nullptr));
        state.finalize_keyboard_report();
    }

    {
        hid_runtime::StateMachine state;
        make_ble_ready(state);
        assert(state.begin_keyboard_report(
                   0, {4, 0, 0, 0, 0, 0}) ==
               hid_runtime::KeyboardReportBeginResult::kPublished);
        assert(state.cancel_keyboard_report(
            state.keyboard_report_snapshot().ticket_id));
        state.finalize_keyboard_report();
        assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kPublished);
    }

    {
        hid_runtime::StateMachine state;
        const auto route = state.route_snapshot();
        hid_runtime::ReportHandles duplicate{};
        duplicate.set(hid_runtime::ReportRole::kKeyboardInput, 13);
        duplicate.set(hid_runtime::ReportRole::kMouseInput, 13);
        assert(state.request_route_ble({
            .expected_authority_epoch = state.authority_epoch(),
            .expected_route_generation = route.generation,
            .ble_generation = 11,
            .connection_handle = 12,
            .present_roles = hid_capability::kInputRoles,
            .required_input_subscriptions = hid_capability::kInputRoles,
            .report_handles = duplicate,
        }).action_result == hid_runtime::RouteTransitionResult::kNotReady);
    }
}

void test_stale_profile_activation_epoch_and_release_snapshot() {
    hid_runtime::StateMachine state;
    make_ble_ready(state, hid_capability::kMouseInput);
    const auto first = state.ble_route_authority_snapshot();
    assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
           hid_runtime::MouseReportBeginResult::kPublished);
    const auto stale_work =
        state.published_report_token(hid_runtime::Interface::kMouse);

    auto forged = first;
    ++forged.profile_activation_epoch;
    assert(!state.retire_ble_route_if_matches(forged));
    assert(state.retire_ble_route_if_matches(first));
    const auto retiring = state.ble_route_authority_snapshot();
    assert(retiring.releasing && !retiring.active);
    assert(retiring.profile_activation_epoch ==
           first.profile_activation_epoch);
    assert(retiring.present_roles == hid_capability::kMouseInput);
    assert(state.complete_ble_route_release_if_matches(retiring));

    make_ble_ready(state, hid_capability::kMouseInput);
    const auto second = state.ble_route_authority_snapshot();
    assert(second.profile_activation_epoch !=
           first.profile_activation_epoch);
    assert(second.report_handles.values == first.report_handles.values);
    assert(!state.ble_work_token_current(hid_runtime::Interface::kMouse,
                                         stale_work));
    assert(!state.process_ble_report(hid_runtime::Interface::kMouse,
                                     stale_work, accept_ble_report, nullptr));
}

void test_activation_epoch_consumer_checks_are_independent() {
    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kMouseInput);
        assert(state.begin_mouse_report(1, 0, 0, 0, 0) ==
               hid_runtime::MouseReportBeginResult::kPublished);
        const auto original =
            state.published_report_token(hid_runtime::Interface::kMouse);
        auto forged = original;
        ++forged.profile_activation_epoch;

        // Keep the stored ticket identical to the supplied token so exact
        // ticket matching cannot hide a missing unsafe_work_current epoch
        // comparison. The active BLE route remains on the original epoch.
        state.set_report_profile_activation_epoch_for_test(
            hid_runtime::Interface::kMouse,
            forged.profile_activation_epoch);
        const auto stored =
            state.published_report_token(hid_runtime::Interface::kMouse);
        assert(stored.ticket_id == original.ticket_id);
        assert(stored.route_generation == original.route_generation);
        assert(stored.transport_generation == original.transport_generation);
        assert(stored.connection_handle == original.connection_handle);
        assert(stored.characteristic_handle == original.characteristic_handle);
        assert(stored.profile_activation_epoch ==
               forged.profile_activation_epoch);
        assert(!state.ble_work_token_current(
            hid_runtime::Interface::kMouse, forged));
    }

    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kInputRoles);
        hid_runtime::ConfirmedHidState hid_state{};
        hid_runtime::SequenceAuthority sequence{};
        assert(state.begin_sequence(&hid_state, &sequence) ==
               hid_runtime::SequenceAdmissionResult::kAccepted);
        assert(state.sequence_authority_current(sequence));
        auto forged = sequence;
        ++forged.profile_activation_epoch;
        assert(forged.generation == sequence.generation);
        assert(forged.authority_epoch == sequence.authority_epoch);
        assert(forged.route_generation == sequence.route_generation);
        assert(forged.release_epoch == sequence.release_epoch);
        assert(forged.active_roles == sequence.active_roles);
        assert(!state.sequence_authority_current(forged));
        state.end_sequence(sequence);
    }
}

void test_profile_activation_epoch_exhaustion_never_wraps() {
    hid_runtime::StateMachine state;
    state.set_next_profile_activation_epoch_for_test(
        std::numeric_limits<hid_runtime::ProfileActivationEpoch>::max());
    make_ble_ready(state, hid_capability::kKeyboardInput);
    const auto final = state.ble_route_authority_snapshot();
    assert(final.profile_activation_epoch ==
           std::numeric_limits<hid_runtime::ProfileActivationEpoch>::max());
    assert(state.retire_ble_route_if_matches(final));
    const auto retiring = state.ble_route_authority_snapshot();
    assert(state.complete_ble_route_release_if_matches(retiring));

    const auto route = state.route_snapshot();
    hid_runtime::ReportHandles handles{};
    handles.set(hid_runtime::ReportRole::kKeyboardInput, 13);
    assert(state.request_route_ble({
        .expected_authority_epoch = state.authority_epoch(),
        .expected_route_generation = route.generation,
        .ble_generation = 11,
        .connection_handle = 12,
        .present_roles = hid_capability::kKeyboardInput,
        .required_input_subscriptions = hid_capability::kKeyboardInput,
        .report_handles = handles,
    }).action_result == hid_runtime::RouteTransitionResult::kNotReady);
}

void test_absent_role_release_is_clean_only_when_proven_clean() {
    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kMouseInput);
        state.begin_release_all();
        const auto release = state.release_all_snapshot();
        assert(release.keyboard ==
               hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
        assert(release.mouse ==
               hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    }

    {
        hid_runtime::StateMachine state;
        make_ble_ready(state, hid_capability::kMouseInput);
        const auto active = state.ble_route_authority_snapshot();
        assert(state.retire_ble_route_if_matches(
            active, true, hid_runtime::Interface::kKeyboard));
        const auto old_activation = state.ble_route_authority_snapshot();
        assert(old_activation.releasing &&
               old_activation.profile_activation_epoch ==
                   active.profile_activation_epoch &&
               old_activation.present_roles == hid_capability::kMouseInput);
        state.begin_release_all();
        const auto release = state.release_all_snapshot();
        assert(release.keyboard ==
               hid_runtime::ReleaseAllInterfaceState::kPending);
        assert(state.host_state_uncertain(
            hid_runtime::Interface::kKeyboard));
        assert(state.safety_required(hid_runtime::Interface::kKeyboard));
    }
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

void verify_lifecycle_veto_during_incoherent_route_publication(
    RoutePublicationInterleave interleave) {
    hid_runtime::StateMachine state;
    prepare_route_publication(state);
    route_publication_runtime = &state;
    route_publication_interleave = interleave;
    state.set_route_generation_published_hook_for_test(
        inject_route_publication_lifecycle);

    const auto outcome = state.request_route_usb();
    state.set_route_generation_published_hook_for_test(nullptr);
    assert(outcome.action_result == hid_runtime::RouteTransitionResult::kNotReady);
    const auto route = state.route_snapshot();
    assert(route.coherent && !route.invalidation_pending);
    assert(route.desired == hid_route::OutputRoute::kNone);
    assert(route.active == hid_route::OutputRoute::kNone);
    assert(route.transition == hid_route::Transition::kStable);
    assert(route.generation == 1);

    const auto status = state.status();
    if (interleave == RoutePublicationInterleave::kSuspend) {
        assert(status.mounted && status.suspended);
    } else if (interleave == RoutePublicationInterleave::kUnmount) {
        assert(!status.mounted && !status.suspended);
    } else if (interleave == RoutePublicationInterleave::kRuntimeFault) {
        assert(status.mounted && !status.suspended);
        assert(!status.keyboard_ready && !status.mouse_ready);
    } else {
        assert(status.mounted && !status.suspended);
        state.set_ready(hid_runtime::Interface::kKeyboard, true);
        state.set_ready(hid_runtime::Interface::kMouse, true);
        assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
        assert(state.request_route_usb().action_result ==
               hid_runtime::RouteTransitionResult::kAccepted);
        assert(state.route_snapshot().generation == 2);
    }
    route_publication_runtime = nullptr;
}

void test_lifecycle_vetoes_incoherent_usb_route_publication() {
    for (const auto interleave :
         {RoutePublicationInterleave::kSuspend,
          RoutePublicationInterleave::kSuspendResume,
          RoutePublicationInterleave::kUnmount,
          RoutePublicationInterleave::kUnmountRemount,
          RoutePublicationInterleave::kRuntimeFault}) {
        verify_lifecycle_veto_during_incoherent_route_publication(interleave);
    }
}

void test_lifecycle_cut_closes_pre_registration_window() {
    hid_runtime::StateMachine state;
    prepare_route_publication(state);
    state.set_before_usb_route_commit_hook_for_test(
        inject_suspend_resume_before_route_registration);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kNotReady);
    const auto route = state.route_snapshot();
    assert(route.coherent && !route.invalidation_pending);
    assert(route.active == hid_route::OutputRoute::kNone);
    assert(route.generation == 0);

    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
    assert(state.route_snapshot().generation == 1);
}

void verify_lifecycle_event_at_publication_release_boundary(bool before_release) {
    hid_runtime::StateMachine state;
    prepare_route_publication(state);
    route_publication_runtime = &state;
    route_publication_interleave = RoutePublicationInterleave::kSuspend;
    if (before_release) {
        state.set_route_usb_publication_before_release_hook_for_test(
            inject_route_publication_lifecycle);
    } else {
        state.set_route_usb_publication_after_release_hook_for_test(
            inject_route_publication_lifecycle);
    }

    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kNotReady);
    const auto route = state.route_snapshot();
    assert(route.coherent && !route.invalidation_pending);
    assert(route.active == hid_route::OutputRoute::kNone);
    assert(route.generation == 2);
    assert(state.status().mounted && state.status().suspended);
    route_publication_runtime = nullptr;
}

void test_lifecycle_event_cannot_cross_publication_release_boundary() {
    verify_lifecycle_event_at_publication_release_boundary(true);
    verify_lifecycle_event_at_publication_release_boundary(false);
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

hid_runtime::UsbLinkStallResult apply_usb_link_stall(
    hid_runtime::StateMachine &state,
    hid_runtime::UsbLinkWatchdogSnapshot snapshot) {
    hid_route::ConditionalInvalidationToken token =
        hid_route::kNoConditionalInvalidationToken;
    return state.on_usb_link_stall(snapshot, &token);
}

void test_sof_heartbeat_wraps_and_watchdog_snapshot_is_exact() {
    hid_runtime::StateMachine state;
    hid_runtime::UsbLinkWatchdogSnapshot snapshot{};
    assert(!state.usb_link_watchdog_snapshot(&snapshot));
    assert(!state.usb_link_watchdog_snapshot(nullptr));

    state.set_sof_heartbeat_for_test(UINT32_MAX);
    state.note_sof_activity();
    assert(state.sof_heartbeat() == 0);
    ready(state);
    assert(state.usb_link_watchdog_snapshot(&snapshot));
    assert(snapshot.attach_generation == state.attach_generation());
    assert(snapshot.authority_epoch == state.authority_epoch());
    assert(snapshot.route_generation == state.route_snapshot().generation);
    assert(snapshot.sof_heartbeat == 0);

    state.note_sof_activity();
    assert(apply_usb_link_stall(state, snapshot) ==
           hid_runtime::UsbLinkStallResult::kStale);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kUsb);
}

void test_usb_link_stall_fences_tickets_sequence_and_preserves_lifecycle_truth() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    hid_runtime::ConfirmedHidState confirmed{};
    hid_runtime::SequenceAuthority sequence{};
    assert(state.begin_sequence(&confirmed, &sequence) ==
           hid_runtime::SequenceAdmissionResult::kAccepted);

    hid_runtime::HidTicketId keyboard_ticket = 0;
    hid_runtime::HidTicketId mouse_ticket = 0;
    assert(state.begin_keyboard_report(
               0, {4, 0, 0, 0, 0, 0}, sequence, &keyboard_ticket, 41) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    assert(state.begin_mouse_report(1, 0, 0, 0, 0, sequence, &mouse_ticket,
                                    41) ==
           hid_runtime::MouseReportBeginResult::kPublished);

    hid_runtime::UsbLinkWatchdogSnapshot snapshot{};
    assert(state.usb_link_watchdog_snapshot(&snapshot));
    const auto authority = state.authority_epoch();
    assert(apply_usb_link_stall(state, snapshot) ==
           hid_runtime::UsbLinkStallResult::kApplied);
    assert(apply_usb_link_stall(state, snapshot) ==
           hid_runtime::UsbLinkStallResult::kStale);

    const auto runtime = state.status();
    assert(runtime.mounted && !runtime.suspended);
    assert(!runtime.keyboard_ready && !runtime.mouse_ready);
    assert(state.usb_lifecycle_snapshot().observed ==
           usb_lifecycle::ObservedState::kMounted);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    assert(state.authority_epoch() == authority + 1U);
    assert(!state.sequence_authority_current(sequence));
    assert(state.sequence_active());

    hid_runtime::KeyboardReportSnapshot keyboard{};
    hid_runtime::MouseReportSnapshot mouse{};
    assert(state.keyboard_report_snapshot(keyboard_ticket, &keyboard));
    assert(keyboard.state == hid_runtime::KeyboardReportTicketState::kCanceled);
    assert(keyboard.outcome ==
           hid_runtime::KeyboardReportTicketOutcome::kAuthorityLost);
    assert(state.mouse_report_snapshot(mouse_ticket, &mouse));
    assert(mouse.state == hid_runtime::MouseReportTicketState::kCanceled);
    assert(mouse.outcome == hid_runtime::MouseReportTicketOutcome::kAuthorityLost);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);

    // Neither canceled ticket reached the host, so no release report is
    // fabricated. Link activity can return, but the old route and sequence
    // authority never resurrect.
    state.note_sof_activity();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
    assert(state.route_snapshot().active == hid_route::OutputRoute::kNone);
    state.end_sequence(sequence);
    assert(state.request_route_usb().action_result ==
           hid_runtime::RouteTransitionResult::kAccepted);
}

void test_usb_link_stall_cancels_public_release_ticket_without_delivery_claim() {
    hid_runtime::StateMachine state;
    ready(state);
    state.begin_release_all();
    const auto before = state.release_all_snapshot();
    assert(before.active);
    hid_runtime::UsbLinkWatchdogSnapshot snapshot{};
    assert(state.usb_link_watchdog_snapshot(&snapshot));
    assert(apply_usb_link_stall(state, snapshot) ==
           hid_runtime::UsbLinkStallResult::kApplied);
    const auto after = state.release_all_snapshot();
    assert(!after.active && after.canceled);
    assert(after.keyboard == hid_runtime::ReleaseAllInterfaceState::kCanceled);
    assert(after.mouse == hid_runtime::ReleaseAllInterfaceState::kCanceled);
    assert(!after.finalized);
}

void test_usb_link_stall_preserves_in_flight_uncertainty_and_rejects_late_owner() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    constexpr hid_runtime::ReportOriginOwnerId old_owner = 73;
    assert(state.begin_keyboard_report(
               0, {4, 0, 0, 0, 0, 0}, {}, nullptr, old_owner) ==
           hid_runtime::KeyboardReportBeginResult::kPublished);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 1);
    const auto old_token =
        state.in_flight_token(hid_runtime::Interface::kKeyboard);
    hid_runtime::UsbLinkWatchdogSnapshot snapshot{};
    assert(state.usb_link_watchdog_snapshot(&snapshot));
    assert(apply_usb_link_stall(state, snapshot) ==
           hid_runtime::UsbLinkStallResult::kApplied);
    assert(state.safety_required(hid_runtime::Interface::kKeyboard));
    assert(state.host_state_uncertain(hid_runtime::Interface::kKeyboard));
    assert(!state.report_in_flight(hid_runtime::Interface::kKeyboard));

    hid_runtime::ReportOriginOwnerId recovered = old_owner;
    assert(!state.report_failed_for_token(
        static_cast<std::uint8_t>(hid_runtime::Interface::kKeyboard),
        old_token, nullptr, 0, &recovered));
    assert(recovered == 0);
    assert(state.safety_required(hid_runtime::Interface::kKeyboard));

    state.note_sof_activity();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 2 && sink.instance == 0 && sink.report[0] == 0 &&
           sink.report[2] == 0);
    assert(state.report_complete(0));
    assert(!state.safety_required(hid_runtime::Interface::kKeyboard));
}

hid_runtime::UsbLinkWatchdogSnapshot writing_stall_snapshot{};

void stall_while_ticket_is_writing(hid_runtime::StateMachine *state) {
    assert(apply_usb_link_stall(*state, writing_stall_snapshot) ==
           hid_runtime::UsbLinkStallResult::kApplied);
}

void test_usb_link_stall_wins_writing_ticket_without_slot_reuse() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.usb_link_watchdog_snapshot(&writing_stall_snapshot));
    state.set_before_ticket_publish_hook_for_test(stall_while_ticket_is_writing);
    assert(state.begin_keyboard_report(
               0, {4, 0, 0, 0, 0, 0}) ==
           hid_runtime::KeyboardReportBeginResult::kAuthorityLost);
    state.set_before_ticket_publish_hook_for_test(nullptr);
    const hid_runtime::KeyboardReportSnapshot canceled =
        state.keyboard_report_snapshot();
    assert(canceled.ticket_id != 0);
    assert(canceled.state == hid_runtime::KeyboardReportTicketState::kCanceled);
    assert(canceled.outcome ==
           hid_runtime::KeyboardReportTicketOutcome::kAuthorityLost);
    assert(state.begin_keyboard_report(0, {5, 0, 0, 0, 0, 0}) ==
           hid_runtime::KeyboardReportBeginResult::kNotReady);
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 0);
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

void test_unresolved_usb_release_cannot_finalize_as_success() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.queue_keyboard_report(0, {4, 0, 0, 0, 0, 0}));
    state.execute(Sink::submit, &sink);
    state.report_complete(0);
    state.begin_release_all();
    auto snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard ==
           hid_runtime::ReleaseAllInterfaceState::kUnresolved);
    assert(snapshot.mouse ==
           hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    state.finalize_release_all();
    snapshot = state.release_all_snapshot();
    assert(snapshot.state ==
           hid_runtime::ReleaseAllTransactionState::kTimedOut);
    assert(!snapshot.success_committed);
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
        .transport_generation = new_token.transport_generation,
        .ticket_id = new_token.ticket_id,
        .release_epoch = new_token.release_epoch,
        .transport = new_token.transport,
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

void audit_usb_failure(hid_runtime::StateMachine *target) {
    auto &state = *target;
    state.set_before_release_finalize_hook_for_test(nullptr);
    assert(state.report_in_flight(hid_runtime::Interface::kKeyboard));
    state.report_failed(0);
    assert(state.release_all_snapshot().state == hid_runtime::ReleaseAllTransactionState::kFailed);
}
struct AuditReleaseWake final : hid_runtime::AuthorityEventSink {
    hid_runtime::StateMachine *state;
    Sink sink;
    bool first = true;
    void signal_hid_authority_change() override {
        if (first) { first=false; state->execute(Sink::submit,&sink); }
    }
};
void audit_usb_public_outcome() {
    hid_runtime::Runtime runtime;
    auto &state = runtime.state_machine();
    Sink sink;
    ready(state);
    assert(state.queue_keyboard_report(0,{4,0,0,0,0,0}));
    state.execute(Sink::submit,&sink);
    state.report_complete(0);
    AuditReleaseWake wake;
    wake.state=&state;
    runtime.bind_authority_event_sink(&wake);
    state.set_before_release_finalize_hook_for_test(audit_usb_failure);
    const auto result=runtime.release_all();
    const auto terminal=state.release_all_snapshot();
    assert(terminal.state == hid_runtime::ReleaseAllTransactionState::kFinalizedFailure);
    assert(!result.success && "public USB success must agree with failed terminal ownership");
}


void test_release_id_nonreuse_and_stale_finalizer() {
    hid_runtime::StateMachine state;
    ready(state);
    const auto first = state.begin_release_all();
    assert(first != 0);
    assert(state.finalize_release_all(first).success_committed);
    const auto second = state.begin_release_all();
    assert(second != first);
    assert(state.finalize_release_all(first).canceled);
    assert(state.release_all_snapshot().id == second);
    assert(state.release_all_snapshot().active);
    assert(state.finalize_release_all(second).success_committed);
    state.set_next_release_id_for_test(UINT64_MAX);
    const auto last = state.begin_release_all();
    assert(last == UINT64_MAX);
    assert(state.finalize_release_all(last).success_committed);
    assert(state.begin_release_all() == 0);
}
void test_stale_usb_failure_cannot_terminalize_replacement_release() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);
    assert(state.queue_keyboard_report(0, {4, 0, 0, 0, 0, 0}));
    state.execute(Sink::submit, &sink);
    state.report_complete(0);
    const auto first = state.begin_release_all();
    state.execute(Sink::submit, &sink);
    const auto old_token = state.in_flight_token(hid_runtime::Interface::kKeyboard);
    assert(state.finalize_release_all(first).success_committed);
    // Compress a complete legacy-epoch wrap while the old transfer is pending.
    // Report metadata retains the nonreused transaction ID across the wrap.
    state.set_release_epoch_for_test(old_token.release_epoch - 1U);
    const auto second = state.begin_release_all();
    const auto before = state.release_all_snapshot();
    assert(second != first && before.release_epoch == old_token.release_epoch);
    assert(before.state == hid_runtime::ReleaseAllTransactionState::kOpen);
    assert(state.report_failed_for_token(0, old_token, nullptr, 0));
    const auto after = state.release_all_snapshot();
    assert(after.id == second);
    assert(after.state == before.state &&
           "old USB failure cannot terminalize a new release owner");
    assert(after.keyboard == before.keyboard);
    assert(!state.finalize_release_all(second).success_committed);
}

int main() {
    test_stale_usb_failure_cannot_terminalize_replacement_release();
    audit_usb_public_outcome();
    test_release_id_nonreuse_and_stale_finalizer();

    test_lifecycle_and_generation_cancellation();
    test_readiness_refresh_after_mount_without_hid_work();
    test_sequence_admission_snapshot_and_producer_exclusion();
    test_revoked_sequence_authority_cannot_create_or_submit_ticket();
    test_sequence_ticket_paused_before_publication_cannot_survive_release();
    test_sequence_generation_does_not_wrap_to_stale_authority();
    test_capability_aware_ble_routes_and_activation_epoch();
    test_stale_profile_activation_epoch_and_release_snapshot();
    test_activation_epoch_consumer_checks_are_independent();
    test_profile_activation_epoch_exhaustion_never_wraps();
    test_absent_role_release_is_clean_only_when_proven_clean();
    test_ble_terminal_visibility_follows_confirmed_state();
    test_mailbox_sequence_exclusion_and_safety_priority();
    test_readiness_refresh_after_reattach();
    test_route_generation_is_independent_and_gates_stale_unsafe_work();
    test_lifecycle_vetoes_incoherent_usb_route_publication();
    test_lifecycle_cut_closes_pre_registration_window();
    test_lifecycle_event_cannot_cross_publication_release_boundary();
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
    test_sof_heartbeat_wraps_and_watchdog_snapshot_is_exact();
    test_usb_link_stall_fences_tickets_sequence_and_preserves_lifecycle_truth();
    test_usb_link_stall_cancels_public_release_ticket_without_delivery_claim();
    test_usb_link_stall_preserves_in_flight_uncertainty_and_rejects_late_owner();
    test_usb_link_stall_wins_writing_ticket_without_slot_reuse();
    test_unmount_preserves_uncertainty_for_fresh_generation_reconciliation();
    test_release_ticket_states_and_historical_submission();
    test_release_ticket_failure_and_lifecycle_cancellation();
    test_unresolved_usb_release_cannot_finalize_as_success();
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
