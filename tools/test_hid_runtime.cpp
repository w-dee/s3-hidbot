#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

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

void ready(hid_runtime::StateMachine &state) {
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kKeyboard, true);
    state.set_ready(hid_runtime::Interface::kMouse, true);
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

    state.on_mount();
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

void test_readiness_refresh_after_reattach() {
    hid_runtime::StateMachine state;

    state.on_mount();
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
    assert(state.queue_mouse_report(0, 10, 0, 0, 0));
    state.execute(Sink::submit, &sink);
    assert(sink.calls == 3 && sink.instance == 1 && sink.report[1] == 10);
}

void test_unmount_clears_old_suspend_safety() {
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
    assert(!state.safety_required(hid_runtime::Interface::kMouse));
    state.on_mount();
    state.set_ready(hid_runtime::Interface::kMouse, true);
    assert(state.attach_generation() != old_attach);
    assert(state.authority_epoch() != old_epoch);
    state.execute(Sink::submit, &sink);
    // An old attach never sends a delayed all-up into a new host attach.
    assert(sink.calls == 1);
}

void test_release_ticket_states_and_historical_submission() {
    hid_runtime::StateMachine state;
    Sink sink;
    ready(state);

    state.begin_release_all();
    auto snapshot = state.release_all_snapshot();
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
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
    assert(snapshot.keyboard == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
    assert(snapshot.mouse == hid_runtime::ReleaseAllInterfaceState::kAlreadyUp);
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
    assert(state.cancel_keyboard_report());
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
    assert(state.cancel_mouse_report());
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
    state.set_ready(hid_runtime::Interface::kMouse, true);

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

}  // namespace

int main() {
    test_lifecycle_and_generation_cancellation();
    test_readiness_refresh_after_mount_without_hid_work();
    test_readiness_refresh_after_reattach();
    test_success_failure_and_release();
    test_partial_release_and_no_delayed_unsafe_replay();
    test_release_barrier_discards_queued_work();
    test_release_all_noop_for_known_all_up();
    test_executor_submission_bound();
    test_authority_epoch_suspend_resume_barrier();
    test_suspend_preserves_safety_and_ignores_late_completion();
    test_unmount_clears_old_suspend_safety();
    test_release_ticket_states_and_historical_submission();
    test_release_ticket_failure_and_lifecycle_cancellation();
    test_release_ticket_partial_and_clean_unmounted();
    test_keyboard_report_ticket_and_confirmed_state();
    test_keyboard_report_ticket_cancellation_and_barriers();
    test_keyboard_report_submit_false_is_not_replayed();
    test_mouse_report_ticket_relative_and_confirmed_state();
    test_mouse_report_ticket_cancellation_and_failure();
    test_mouse_release_all_during_in_flight();
    return 0;
}
