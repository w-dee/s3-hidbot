#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "hid_sequence/hid_sequence.hpp"

namespace {

struct Clock {
    std::uint64_t value = 0;
    static std::uint64_t now(void *context) {
        auto *clock = static_cast<Clock *>(context);
        const std::uint64_t result = clock->value;
        clock->value += 1000;
        return result;
    }
};

class Backend final : public hid_sequence::Backend {
  public:
    hid_sequence::AdmissionResult begin_sequence(
        hid_sequence::HidState *initial_state,
        hid_sequence::ExecutionAuthority *authority) override {
        ++admissions;
        *initial_state = initial;
        *authority = {.generation = generation,
                      .authority_epoch = epoch,
                      .release_epoch = release_epoch};
        owned = true;
        if (wake_during_admission && controller != nullptr) {
            controller->run_for_test();
        }
        return admission;
    }
    bool authority_current(hid_sequence::ExecutionAuthority authority) const override {
        return owned && authority.generation == generation &&
               authority.authority_epoch == epoch &&
               authority.release_epoch == release_epoch;
    }
    void end_sequence(hid_sequence::ExecutionAuthority authority) override {
        if (authority_current(authority)) owned = false;
    }
    void revoke_sequence() override { owned = false; }
    hid_sequence::ReportResult keyboard_report(
        hid_sequence::ExecutionAuthority authority,
        std::uint64_t originating_local_owner_id,
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes) override {
        if (abort_before_ticket && controller != nullptr) {
            abort_before_ticket = false;
            controller->abort();
            release_completed = true;
        }
        if (!authority_current(authority)) {
            return hid_sequence::ReportResult::kAuthorityLost;
        }
        if (observe_running_status && controller != nullptr) {
            assert(controller->status(observed_sequence_id, &observed_status));
            assert(observed_status.state == hid_sequence::State::kRunning);
            assert(observed_status.started);
        }
        report_times.push_back(clock->value);
        report_owner_ids.push_back(originating_local_owner_id);
        keyboard_modifiers.push_back(modifiers);
        keyboard_keys.push_back(keycodes);
        clock->value += report_work_us;
        if (revoke_during_report) owned = false;
        return report_result;
    }
    hid_sequence::ReportResult mouse_report(
        hid_sequence::ExecutionAuthority authority,
        std::uint64_t originating_local_owner_id,
        std::uint8_t buttons) override {
        if (!authority_current(authority)) {
            return hid_sequence::ReportResult::kAuthorityLost;
        }
        report_times.push_back(clock->value);
        report_owner_ids.push_back(originating_local_owner_id);
        mouse_buttons.push_back(buttons);
        clock->value += report_work_us;
        return report_result;
    }
    void request_safety_release() override { ++safety_releases; }

    Clock *clock = nullptr;
    hid_sequence::HidState initial{};
    hid_sequence::AdmissionResult admission = hid_sequence::AdmissionResult::kAccepted;
    hid_sequence::ReportResult report_result = hid_sequence::ReportResult::kAccepted;
    std::uint32_t epoch = 7;
    std::uint32_t generation = 3;
    std::uint32_t release_epoch = 5;
    std::uint64_t report_work_us = 0;
    int admissions = 0;
    int safety_releases = 0;
    bool owned = false;
    bool wake_during_admission = false;
    bool revoke_during_report = false;
    bool abort_before_ticket = false;
    bool release_completed = false;
    bool observe_running_status = false;
    std::int32_t observed_sequence_id = 0;
    hid_sequence::Status observed_status{};
    hid_sequence::Controller *controller = nullptr;
    std::vector<std::uint64_t> report_times;
    std::vector<std::uint64_t> report_owner_ids;
    std::vector<std::uint8_t> keyboard_modifiers;
    std::vector<std::array<std::uint8_t, 6>> keyboard_keys;
    std::vector<std::uint8_t> mouse_buttons;
};

void parser_contract() {
    hid_sequence::Plan plan{};
    const hid_sequence::HidState empty{};
    assert(hid_sequence::parse("d10;kp4;kr4;w200;mpL;mrL", empty, &plan));
    assert(plan.count == 6);
    assert(plan.scheduled_duration_ms == 230);
    assert(hid_sequence::parse(
        "d0;w0;kp224;kp225;kp226;kp227;kp228;kp229;kp230;kp231;"
        "kr224;kr225;kr226;kr227;kr228;kr229;kr230;kr231;"
        "mpL;mrL;mpR;mrR;mpM;mrM;mpB;mrB;mpF;mrF",
        empty, &plan));
    assert(hid_sequence::parse("d10;kp4", empty, &plan));
    assert(plan.scheduled_duration_ms == 0);
    assert(hid_sequence::parse("d10;kp4;w200", empty, &plan));
    assert(plan.scheduled_duration_ms == 210);
    assert(hid_sequence::parse("d10;w200;kp4", empty, &plan));
    assert(plan.scheduled_duration_ms == 200);

    for (const char *invalid : {"", ";", "kp4;", "kp04", "kp3", "kp165",
                                "kp222", "kp232", "d1001", "w2001", "w-1",
                                "w00000", "d9999", "kp0004", "mpX", "MP L",
                                "x1", "kp4;;kr4"}) {
        assert(!hid_sequence::parse(invalid, empty, &plan));
    }
    assert(!hid_sequence::parse("w2000;w501", empty, &plan));

    assert(hid_sequence::parse("kp4;kp5;kp6;kp7;kp8;kp9", empty, &plan));
    assert(!hid_sequence::parse("kp4;kp5;kp6;kp7;kp8;kp9;kp10", empty, &plan));
    hid_sequence::HidState held{};
    held.keycodes = {4, 5, 6, 7, 8, 9};
    assert(hid_sequence::parse("kp4;kr99;kr4;kp10", held, &plan));

    std::string boundary;
    for (int index = 0; index < 43; ++index) {
        if (!boundary.empty()) boundary += ';';
        boundary += "kp231";
    }
    for (int index = 0; index < 21; ++index) {
        boundary += ";d0";
    }
    assert(boundary.size() == 320);
    assert(hid_sequence::parse(boundary, empty, &plan));
    assert(plan.count == 64);
    boundary += ";w0";
    assert(!hid_sequence::parse(boundary, empty, &plan));
}

void local_delay_does_not_catch_up() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.report_work_us = 50000;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(17, "d10;kp4;w200;kr4") ==
           hid_sequence::AdmissionResult::kAccepted);
    assert(controller.start(18, "w0") == hid_sequence::AdmissionResult::kBusy);
    controller.run_for_test();
    assert(backend.report_times.size() == 2);
    // First-report work (50 ms), its local default delay (10 ms), and the
    // explicit wait (200 ms) all elapse before the second report. A global
    // catch-up schedule would compress this after the injected 50 ms overrun.
    assert(backend.report_times[1] - backend.report_times[0] >= 260000);
    hid_sequence::Status status{};
    assert(controller.status(17, &status));
    assert(status.state == hid_sequence::State::kCompleted);
    assert(status.executed == 4);
    assert(backend.safety_releases == 0);
    assert(!backend.owned);
}

void state_and_failure_contract() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.initial.keycodes[0] = 4;
    backend.initial.mouse_buttons = 1;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(23, "kp4;kr99;kp224;mpL;mrR") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    assert(backend.keyboard_keys.size() == 3);
    assert(backend.keyboard_keys[0][0] == 4);
    assert(backend.keyboard_modifiers.back() == 1);
    assert(backend.mouse_buttons.size() == 2);
    assert(backend.mouse_buttons[0] == 1);
    assert(backend.mouse_buttons[1] == 1);

    backend.report_result = hid_sequence::ReportResult::kNotReady;
    assert(controller.start(24, "kp5") == hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(24, &status));
    assert(status.state == hid_sequence::State::kFailed);
    assert(status.code == hid_sequence::TerminalCode::kHidNotReady);
    assert(status.failed_token_present && status.failed_token == 0);
    assert(backend.safety_releases == 1);
    controller.retire_owner(1);
    assert(!controller.status(24, &status));
}

void admission_abort_and_authority_contract() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));

    assert(controller.start(30, "kp4;not-an-operation") ==
           hid_sequence::AdmissionResult::kInvalid);
    assert(backend.report_times.empty());
    assert(!backend.owned);

    assert(controller.start(31, "w200;kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    hid_sequence::Status status{};
    assert(controller.status(31, &status));
    assert(status.state == hid_sequence::State::kAccepted);
    assert(!status.started && status.executed == 0);
    controller.abort();
    controller.run_for_test();
    assert(controller.status(31, &status));
    assert(status.state == hid_sequence::State::kAborted);
    assert(status.started && status.executed == 0);
    assert(status.failed_token_present && status.failed_token == 0);
    assert(status.code == hid_sequence::TerminalCode::kSequenceAborted);
    assert(backend.safety_releases == 1);
    assert(backend.report_times.empty());

    assert(controller.start(32, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    ++backend.epoch;
    controller.run_for_test();
    assert(controller.status(32, &status));
    assert(status.state == hid_sequence::State::kAborted);
    assert(status.code == hid_sequence::TerminalCode::kSessionMismatch);
    assert(backend.safety_releases == 2);
    controller.retire_owner(1);
    assert(!controller.status(32, &status));
}

void execution_deadline_contract() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.report_work_us =
        static_cast<std::uint64_t>(hid_sequence::kExecutionDeadlineMs) * 1000U;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(40, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(40, &status));
    assert(status.state == hid_sequence::State::kFailed);
    assert(status.code == hid_sequence::TerminalCode::kSequenceTimeout);
    assert(status.executed == 0);
    assert(status.failed_token_present && status.failed_token == 0);
    assert(backend.safety_releases == 1);
}

void revoked_report_cannot_complete() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.revoke_during_report = true;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(50, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(50, &status));
    assert(status.state == hid_sequence::State::kAborted);
    assert(status.executed == 0);
    assert(status.failed_token_present && status.failed_token == 0);
    assert(backend.safety_releases == 1);
}

void abort_and_release_before_ticket_creation_rejects_stale_work() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.abort_before_ticket = true;
    hid_sequence::Controller controller;
    backend.controller = &controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(51, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(51, &status));
    assert(backend.release_completed);
    assert(backend.report_times.empty());
    assert(status.state == hid_sequence::State::kAborted);
    assert(status.executed == 0);
    assert(status.failed_token_present && status.failed_token == 0);
    assert(status.code == hid_sequence::TerminalCode::kSequenceAborted);
}

void stale_wake_cannot_consume_new_admission() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    backend.controller = &controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(60, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    controller.abort();
    backend.wake_during_admission = true;
    assert(controller.start(61, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    assert(controller.active());
    controller.run_for_test();
    assert(backend.report_times.size() == 1);
    hid_sequence::Status status{};
    assert(controller.status(61, &status));
    assert(status.state == hid_sequence::State::kCompleted);
    assert(status.started && status.executed == 1);
}

void bounded_wait_arithmetic() {
    auto decision = hid_sequence::wait_decision(1001, 1000, 5000);
    assert(decision.wait_complete && !decision.execution_expired &&
           decision.block_us == 0);
    decision = hid_sequence::wait_decision(5000, 6000, 5000);
    assert(decision.execution_expired && !decision.wait_complete &&
           decision.block_us == 0);
    decision = hid_sequence::wait_decision(999, 1999, 5000);
    assert(!decision.wait_complete && !decision.execution_expired &&
           decision.block_us == 1000);
    decision = hid_sequence::wait_decision(0, 2000000, 4000000);
    assert(decision.block_us == 2000000);
    decision = hid_sequence::wait_decision(3900000, 5900000, 4000000);
    assert(decision.block_us == 100000);
    decision = hid_sequence::wait_decision(4100000, 5900000, 4000000);
    assert(decision.execution_expired && decision.block_us == 0);
}

void controller_generation_does_not_wrap() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    controller.set_next_generation_for_test(
        std::numeric_limits<std::uint32_t>::max());
    assert(controller.start(70, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    assert(controller.start(71, "w0") ==
           hid_sequence::AdmissionResult::kNotReady);
}

struct TerminalRaceClock {
    hid_sequence::Controller *controller = nullptr;
    std::uint64_t owner_id = 0;
    unsigned calls = 0;
    bool retire_owner = false;
    bool force_timeout = false;

    static std::uint64_t now(void *context) {
        auto *clock = static_cast<TerminalRaceClock *>(context);
        ++clock->calls;
        if (clock->calls == 4) {
            if (clock->force_timeout) return 4001000;
            if (clock->retire_owner) {
                clock->controller->retire_owner(clock->owner_id);
            } else {
                clock->controller->abort();
            }
        }
        return static_cast<std::uint64_t>(clock->calls) * 1000U;
    }
};

void terminal_claim_arbitrates_completion_revocation_and_timeout() {
    for (const bool retire_owner : {false, true}) {
        Clock backend_clock{};
        Backend backend{};
        TerminalRaceClock clock{};
        hid_sequence::Controller controller;
        backend.clock = &backend_clock;
        backend.controller = &controller;
        clock.controller = &controller;
        clock.owner_id = 1;
        clock.retire_owner = retire_owner;
        assert(controller.initialize(&backend, TerminalRaceClock::now, &clock));
        assert(controller.start(80, "kp4") ==
               hid_sequence::AdmissionResult::kAccepted);
        controller.run_for_test();
        hid_sequence::Status status{};
        if (retire_owner) {
            assert(!controller.status(80, &status));
        } else {
            assert(controller.status(80, &status));
            assert(status.state == hid_sequence::State::kAborted);
            assert(status.started && status.executed == 0);
            assert(status.failed_token_present && status.failed_token == 0);
            assert(status.code == hid_sequence::TerminalCode::kSequenceAborted);
        }
        assert(backend.safety_releases == 1);
    }

    Clock backend_clock{};
    Backend backend{};
    TerminalRaceClock clock{};
    hid_sequence::Controller controller;
    backend.clock = &backend_clock;
    clock.controller = &controller;
    clock.force_timeout = true;
    assert(controller.initialize(&backend, TerminalRaceClock::now, &clock));
    assert(controller.start(81, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(81, &status));
    assert(status.state == hid_sequence::State::kFailed);
    assert(status.code == hid_sequence::TerminalCode::kSequenceTimeout);
    assert(status.executed == 0 && status.failed_token == 0);
    assert(backend.safety_releases == 1);
}

void status_snapshot_is_coherent_across_lifecycle() {
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    backend.observe_running_status = true;
    backend.observed_sequence_id = 82;
    hid_sequence::Controller controller;
    backend.controller = &controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(82, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    hid_sequence::Status accepted{};
    assert(controller.status(82, &accepted));
    assert(accepted.state == hid_sequence::State::kAccepted &&
           !accepted.started && accepted.executed == 0);
    controller.run_for_test();
    assert(backend.observed_status.state == hid_sequence::State::kRunning &&
           backend.observed_status.started &&
           backend.observed_status.executed == 0);
    hid_sequence::Status completed{};
    assert(controller.status(82, &completed));
    assert(completed.state == hid_sequence::State::kCompleted &&
           completed.started && completed.executed == 1 &&
           !completed.failed_token_present &&
           completed.code == hid_sequence::TerminalCode::kNone);
    controller.abort();
    assert(controller.status(82, &completed));
    assert(completed.state == hid_sequence::State::kCompleted);

    Clock abort_clock{};
    Backend abort_backend{};
    abort_backend.clock = &abort_clock;
    hid_sequence::Controller aborted_controller;
    assert(aborted_controller.initialize(&abort_backend, Clock::now,
                                         &abort_clock));
    assert(aborted_controller.start(83, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    aborted_controller.abort();
    hid_sequence::Status aborted{};
    assert(aborted_controller.status(83, &aborted));
    assert(aborted.state == hid_sequence::State::kAborted &&
           aborted.started && aborted.executed == 0 &&
           aborted.failed_token_present && aborted.failed_token == 0 &&
           aborted.code == hid_sequence::TerminalCode::kSequenceAborted);
    aborted_controller.run_for_test();
    assert(aborted_controller.status(83, &aborted));
    assert(aborted.state == hid_sequence::State::kAborted);
}

void local_owner_identity_survives_stale_cleanup_and_id_reuse() {
    constexpr std::uint64_t owner_a =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    constexpr std::uint64_t owner_b = owner_a + 1U;
    constexpr std::int32_t reused_sequence_id = 7;

    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));
    assert(controller.start(0, reused_sequence_id, "w0") ==
           hid_sequence::AdmissionResult::kNotReady);

    assert(controller.start(owner_a, reused_sequence_id, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(owner_a, reused_sequence_id, &status));
    assert(status.state == hid_sequence::State::kCompleted);
    assert(!controller.status(owner_b, reused_sequence_id, &status));

    const std::uint64_t delayed_owner_a_cleanup = owner_a;
    controller.retire_owner(owner_a);
    assert(!controller.status(owner_a, reused_sequence_id, &status));
    assert(!controller.status(owner_b, reused_sequence_id, &status));

    assert(controller.start(owner_b, reused_sequence_id, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    assert(controller.status(owner_b, reused_sequence_id, &status));
    assert(status.state == hid_sequence::State::kAccepted);

    controller.retire_owner(delayed_owner_a_cleanup);
    assert(controller.active());
    assert(controller.status(owner_b, reused_sequence_id, &status));
    assert(status.state == hid_sequence::State::kAccepted);
    controller.run_for_test();
    assert(controller.status(owner_b, reused_sequence_id, &status));
    assert(status.state == hid_sequence::State::kCompleted);

    controller.retire_owner(delayed_owner_a_cleanup);
    assert(controller.status(owner_b, reused_sequence_id, &status));
    assert(status.state == hid_sequence::State::kCompleted);
}

void sequence_reports_preserve_high_bit_local_owner() {
    constexpr std::uint64_t owner =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
        17U;
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));

    assert(controller.start(owner, 91, "kp4;mpL") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    assert(backend.report_owner_ids.size() == 2);
    assert(backend.report_owner_ids[0] == owner);
    assert(backend.report_owner_ids[1] == owner);
}

void retired_owner_late_wake_cannot_republish_for_new_owner() {
    constexpr std::uint64_t owner_a = 41;
    constexpr std::uint64_t owner_b = 42;
    Clock clock{};
    Backend backend{};
    backend.clock = &clock;
    hid_sequence::Controller controller;
    assert(controller.initialize(&backend, Clock::now, &clock));

    assert(controller.start(owner_a, 7, "kp4") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.retire_owner(owner_a);
    assert(!controller.status(owner_a, 7, nullptr));
    controller.run_for_test();
    assert(!controller.status(owner_b, 7, nullptr));

    assert(controller.start(owner_b, 7, "w0") ==
           hid_sequence::AdmissionResult::kAccepted);
    controller.run_for_test();
    hid_sequence::Status status{};
    assert(controller.status(owner_b, 7, &status));
    assert(status.state == hid_sequence::State::kCompleted);
}

}  // namespace

int main() {
    parser_contract();
    local_delay_does_not_catch_up();
    state_and_failure_contract();
    admission_abort_and_authority_contract();
    execution_deadline_contract();
    revoked_report_cannot_complete();
    abort_and_release_before_ticket_creation_rejects_stale_work();
    stale_wake_cannot_consume_new_admission();
    bounded_wait_arithmetic();
    controller_generation_does_not_wrap();
    terminal_claim_arbitrates_completion_revocation_and_timeout();
    status_snapshot_is_coherent_across_lifecycle();
    local_owner_identity_survives_stale_cleanup_and_id_reuse();
    sequence_reports_preserve_high_bit_local_owner();
    retired_owner_late_wake_cannot_republish_for_new_owner();
    return 0;
}
