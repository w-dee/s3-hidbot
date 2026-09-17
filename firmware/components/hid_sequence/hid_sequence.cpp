#include "hid_sequence/hid_sequence.hpp"

#include <algorithm>
#include <limits>

#ifndef HID_SEQUENCE_NATIVE_TEST
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace hid_sequence {
namespace {

bool ordinary_usage(std::uint16_t value) {
    return (value >= 4 && value <= 164) || (value >= 176 && value <= 221);
}

bool modifier_usage(std::uint16_t value) {
    return value >= 224 && value <= 231;
}

bool parse_number(std::string_view text, std::uint16_t maximum,
                  std::uint16_t *value) {
    if (value == nullptr || text.empty() || text.size() > 4 ||
        (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    std::uint32_t parsed = 0;
    for (char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10U + static_cast<std::uint32_t>(character - '0');
        if (parsed > maximum) {
            return false;
        }
    }
    *value = static_cast<std::uint16_t>(parsed);
    return true;
}

std::uint8_t button_bit(char button) {
    switch (button) {
        case 'L': return 1U << 0;
        case 'R': return 1U << 1;
        case 'M': return 1U << 2;
        case 'B': return 1U << 3;
        case 'F': return 1U << 4;
        default: return 0;
    }
}

bool apply_transition(const Operation &operation, HidState *state) {
    if (state == nullptr) {
        return false;
    }
    if (operation.kind == OperationKind::kMousePress ||
        operation.kind == OperationKind::kMouseRelease) {
        const std::uint8_t bit = static_cast<std::uint8_t>(operation.value);
        if (operation.kind == OperationKind::kMousePress) {
            state->mouse_buttons |= bit;
        } else {
            state->mouse_buttons &= static_cast<std::uint8_t>(~bit);
        }
        return true;
    }
    if (operation.kind != OperationKind::kKeyPress &&
        operation.kind != OperationKind::kKeyRelease) {
        return true;
    }
    const std::uint16_t usage = operation.value;
    if (modifier_usage(usage)) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1U << (usage - 224U));
        if (operation.kind == OperationKind::kKeyPress) {
            state->modifiers |= bit;
        } else {
            state->modifiers &= static_cast<std::uint8_t>(~bit);
        }
        return true;
    }
    std::size_t found = state->keycodes.size();
    for (std::size_t index = 0; index < state->keycodes.size(); ++index) {
        if (state->keycodes[index] == usage) {
            found = index;
            break;
        }
    }
    if (operation.kind == OperationKind::kKeyPress) {
        if (found != state->keycodes.size()) {
            return true;
        }
        std::size_t empty = state->keycodes.size();
        for (std::size_t index = 0; index < state->keycodes.size(); ++index) {
            if (state->keycodes[index] == 0) {
                empty = index;
                break;
            }
        }
        if (empty == state->keycodes.size()) {
            return false;
        }
        while (empty > 0 && state->keycodes[empty - 1] > usage) {
            state->keycodes[empty] = state->keycodes[empty - 1];
            --empty;
        }
        state->keycodes[empty] = static_cast<std::uint8_t>(usage);
    } else if (found != state->keycodes.size()) {
        for (std::size_t index = found; index + 1 < state->keycodes.size(); ++index) {
            state->keycodes[index] = state->keycodes[index + 1];
        }
        state->keycodes.back() = 0;
    }
    return true;
}

bool is_hid(OperationKind kind) {
    return kind == OperationKind::kKeyPress ||
           kind == OperationKind::kKeyRelease ||
           kind == OperationKind::kMousePress ||
           kind == OperationKind::kMouseRelease;
}

bool allocate_generation(std::atomic<std::uint32_t> *next,
                         std::uint32_t *generation) {
    std::uint32_t candidate = next->load(std::memory_order_acquire);
    while (candidate != 0) {
        const std::uint32_t successor =
            candidate == std::numeric_limits<std::uint32_t>::max()
                ? 0
                : candidate + 1;
        if (next->compare_exchange_weak(candidate, successor,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            *generation = candidate;
            return true;
        }
    }
    return false;
}

constexpr std::uint32_t kStatusStateMask = 0x7U;
constexpr std::uint32_t kStatusExecutedShift = 3U;
constexpr std::uint32_t kStatusExecutedMask = 0x7fU;
constexpr std::uint32_t kStatusFailurePresent = 1U << 10U;
constexpr std::uint32_t kStatusFailedTokenShift = 11U;
constexpr std::uint32_t kStatusFailedTokenMask = 0x3fU;
constexpr std::uint32_t kStatusCodeShift = 17U;
constexpr std::uint32_t kStatusCodeMask = 0xfU;

constexpr std::uint32_t encode_status(State state, std::uint8_t executed,
                                      bool failed_token_present,
                                      std::uint8_t failed_token,
                                      TerminalCode code) {
    return static_cast<std::uint32_t>(state) |
           ((static_cast<std::uint32_t>(executed) & kStatusExecutedMask)
            << kStatusExecutedShift) |
           (failed_token_present ? kStatusFailurePresent : 0U) |
           ((static_cast<std::uint32_t>(failed_token) & kStatusFailedTokenMask)
            << kStatusFailedTokenShift) |
           ((static_cast<std::uint32_t>(code) & kStatusCodeMask)
            << kStatusCodeShift);
}

constexpr State decode_state(std::uint32_t status) {
    return static_cast<State>(status & kStatusStateMask);
}

constexpr std::uint8_t decode_executed(std::uint32_t status) {
    return static_cast<std::uint8_t>(
        (status >> kStatusExecutedShift) & kStatusExecutedMask);
}

constexpr bool decode_failed_token_present(std::uint32_t status) {
    return (status & kStatusFailurePresent) != 0;
}

constexpr std::uint8_t decode_failed_token(std::uint32_t status) {
    return static_cast<std::uint8_t>(
        (status >> kStatusFailedTokenShift) & kStatusFailedTokenMask);
}

constexpr TerminalCode decode_terminal_code(std::uint32_t status) {
    return static_cast<TerminalCode>(
        (status >> kStatusCodeShift) & kStatusCodeMask);
}

constexpr bool terminal_state(State state) {
    return state == State::kCompleted || state == State::kFailed ||
           state == State::kAborted;
}

constexpr std::uint32_t owner_low(std::uint64_t owner) {
    return static_cast<std::uint32_t>(owner);
}

constexpr std::uint32_t owner_high(std::uint64_t owner) {
    return static_cast<std::uint32_t>(owner >> 32U);
}

#ifndef HID_SEQUENCE_NATIVE_TEST
constexpr std::size_t kTaskStackBytes = 4096;
constexpr std::size_t kTaskStackDepth = kTaskStackBytes / sizeof(StackType_t);
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 2;
StaticTask_t s_task_storage;
StackType_t s_task_stack[kTaskStackDepth]{};
TaskHandle_t s_task = nullptr;
#endif

}  // namespace

bool parse(std::string_view code, const HidState &initial_state, Plan *plan) {
    if (plan == nullptr || code.empty() || code.size() > kMaximumCodeBytes) {
        return false;
    }
    Plan candidate{};
    candidate.initial_state = initial_state;
    HidState simulated = initial_state;
    std::size_t offset = 0;
    while (offset < code.size()) {
        if (candidate.count == kMaximumTokens) {
            return false;
        }
        const std::size_t delimiter = code.find(';', offset);
        const std::size_t end = delimiter == std::string_view::npos
                                    ? code.size() : delimiter;
        const std::string_view token = code.substr(offset, end - offset);
        if (token.empty()) {
            return false;
        }
        Operation operation{};
        std::uint16_t value = 0;
        if (token.front() == 'd') {
            if (!parse_number(token.substr(1), kMaximumDefaultDelayMs, &value)) return false;
            operation = {OperationKind::kDefaultDelay, value};
        } else if (token.front() == 'w') {
            if (!parse_number(token.substr(1), kMaximumWaitMs, &value)) return false;
            operation = {OperationKind::kWait, value};
        } else if (token.size() >= 3 && (token.substr(0, 2) == "kp" || token.substr(0, 2) == "kr")) {
            if (token.size() > 5 || !parse_number(token.substr(2), 231, &value) ||
                (!ordinary_usage(value) && !modifier_usage(value))) return false;
            operation = {token[1] == 'p' ? OperationKind::kKeyPress
                                         : OperationKind::kKeyRelease, value};
        } else if (token.size() == 3 && (token.substr(0, 2) == "mp" || token.substr(0, 2) == "mr")) {
            const std::uint8_t bit = button_bit(token[2]);
            if (bit == 0) return false;
            operation = {token[1] == 'p' ? OperationKind::kMousePress
                                         : OperationKind::kMouseRelease, bit};
        } else {
            return false;
        }
        if (!apply_transition(operation, &simulated)) {
            return false;
        }
        if (operation.kind == OperationKind::kKeyPress ||
            operation.kind == OperationKind::kKeyRelease) {
            candidate.required_roles |= hid_capability::kKeyboardInput;
        } else if (operation.kind == OperationKind::kMousePress ||
                   operation.kind == OperationKind::kMouseRelease) {
            candidate.required_roles |= hid_capability::kMouseInput;
        }
        candidate.operations[candidate.count++] = operation;
        if (delimiter == std::string_view::npos) break;
        offset = delimiter + 1;
        if (offset == code.size()) return false;
    }

    std::uint32_t default_delay = 0;
    std::uint32_t duration = 0;
    for (std::size_t index = 0; index < candidate.count; ++index) {
        const Operation &operation = candidate.operations[index];
        if (operation.kind == OperationKind::kDefaultDelay) {
            default_delay = operation.value;
        } else if (operation.kind == OperationKind::kWait) {
            duration += operation.value;
        } else if (is_hid(operation.kind) && index + 1 < candidate.count) {
            duration += default_delay;
        }
        if (duration > kMaximumScheduledDurationMs) return false;
    }
    candidate.scheduled_duration_ms = duration;
    *plan = candidate;
    return true;
}

WaitDecision wait_decision(std::uint64_t now, std::uint64_t wait_deadline,
                           std::uint64_t execution_deadline) {
    if (now >= execution_deadline) {
        return WaitDecision{.execution_expired = true};
    }
    if (now >= wait_deadline) {
        return WaitDecision{.wait_complete = true};
    }
    return WaitDecision{
        .block_us = std::min(wait_deadline - now, execution_deadline - now),
    };
}

const char *state_name(State state) {
    switch (state) {
        case State::kAccepted: return "accepted";
        case State::kRunning: return "running";
        case State::kCompleted: return "completed";
        case State::kFailed: return "failed";
        case State::kAborted: return "aborted";
    }
    return "failed";
}

const char *terminal_code_name(TerminalCode code) {
    switch (code) {
        case TerminalCode::kNone: return nullptr;
        case TerminalCode::kHidBusy: return "HID_BUSY";
        case TerminalCode::kHidNotReady: return "HID_NOT_READY";
        case TerminalCode::kHidSafetyPending: return "HID_SAFETY_PENDING";
        case TerminalCode::kSessionMismatch: return "SESSION_MISMATCH";
        case TerminalCode::kSequenceTimeout: return "SEQUENCE_TIMEOUT";
        case TerminalCode::kSequenceAborted: return "SEQUENCE_ABORTED";
    }
    return "SEQUENCE_ABORTED";
}

bool Controller::initialize(Backend *backend, NowFn now, void *now_context) {
    if (backend == nullptr || now == nullptr || initialized_.load()) return false;
    backend_ = backend;
    now_ = now;
    now_context_ = now_context;
#ifndef HID_SEQUENCE_NATIVE_TEST
    s_task = xTaskCreateStatic(task_entry, "hid_sequence", kTaskStackDepth, this,
                               kTaskPriority, s_task_stack, &s_task_storage);
    if (s_task == nullptr) return false;
#endif
    initialized_.store(true, std::memory_order_release);
    return true;
}

AdmissionResult Controller::start(std::uint64_t local_owner_id,
                                  std::int32_t sequence_id,
                                  std::string_view code) {
    if (!initialized_.load(std::memory_order_acquire) || local_owner_id == 0) {
        return AdmissionResult::kNotReady;
    }
    std::uint32_t generation = 0;
    if (!allocate_generation(&next_generation_, &generation)) {
        return AdmissionResult::kNotReady;
    }
    std::uint32_t unreserved = 0;
    if (!reserved_generation_.compare_exchange_strong(
            unreserved, generation, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return AdmissionResult::kBusy;
    }
    HidState initial{};
    ExecutionAuthority authority{};
    const AdmissionResult admission = backend_->begin_sequence(&initial, &authority);
    if (admission != AdmissionResult::kAccepted) {
        reserved_generation_.store(0, std::memory_order_release);
        return admission;
    }
    Plan candidate{};
    if (!parse(code, initial, &candidate)) {
        backend_->end_sequence(authority);
        reserved_generation_.store(0, std::memory_order_release);
        return AdmissionResult::kInvalid;
    }
    if (!hid_capability::is_subset(candidate.required_roles,
                                   authority.active_roles)) {
        backend_->end_sequence(authority);
        reserved_generation_.store(0, std::memory_order_release);
        return AdmissionResult::kUnsupportedOperation;
    }
    plan_ = candidate;
    authority_ = authority;
    active_local_owner_id_ = local_owner_id;
    admission_us_ = now_(now_context_);
    cancel_requested_.store(
        canceled_generation_.load(std::memory_order_acquire) == generation,
        std::memory_order_release);
    // Withdraw the retained record while its identity and packed lifecycle
    // are replaced. Status readers validate this generation before and after
    // sampling, so they cannot combine a new sequence ID with the preceding
    // sequence's terminal word.
    status_generation_.store(0, std::memory_order_release);
    sequence_id_.store(sequence_id, std::memory_order_relaxed);
    status_word_.store(
        encode_status(State::kAccepted, 0, false, 0, TerminalCode::kNone),
        std::memory_order_relaxed);
    status_owner_low_.store(owner_low(local_owner_id), std::memory_order_relaxed);
    status_owner_high_.store(owner_high(local_owner_id), std::memory_order_relaxed);
    // The generation is the complete-record publication commit.
    status_generation_.store(generation, std::memory_order_release);
    runnable_generation_.store(generation, std::memory_order_release);
#ifndef HID_SEQUENCE_NATIVE_TEST
    xTaskNotifyGive(s_task);
#endif
    return AdmissionResult::kAccepted;
}

bool Controller::status(std::uint64_t local_owner_id,
                        std::int32_t sequence_id,
                        Status *status) {
    const std::uint32_t generation =
        status_generation_.load(std::memory_order_acquire);
    const std::uint32_t observed_owner_low =
        status_owner_low_.load(std::memory_order_relaxed);
    const std::uint32_t observed_owner_high =
        status_owner_high_.load(std::memory_order_relaxed);
    if (status == nullptr || generation == 0 || local_owner_id == 0 ||
        observed_owner_low != owner_low(local_owner_id) ||
        observed_owner_high != owner_high(local_owner_id) ||
        sequence_id_.load(std::memory_order_acquire) != sequence_id) return false;
    status->sequence_id = sequence_id;
    const std::uint32_t observed = status_word_.load(std::memory_order_acquire);
    const State observed_state = decode_state(observed);
    status->state = observed_state;
    status->started = observed_state != State::kAccepted;
    status->executed = decode_executed(observed);
    const bool terminal = observed_state == State::kFailed ||
                          observed_state == State::kAborted;
    status->failed_token_present = terminal &&
        decode_failed_token_present(observed);
    status->failed_token = terminal
        ? decode_failed_token(observed) : 0;
    status->code = terminal
        ? decode_terminal_code(observed) : TerminalCode::kNone;
    return status_generation_.load(std::memory_order_acquire) == generation;
}

void Controller::abort_generation(std::uint32_t generation) {
    if (generation == 0 ||
        reserved_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    canceled_generation_.store(generation, std::memory_order_release);
    if (reserved_generation_.load(std::memory_order_acquire) != generation) {
        return;
    }
    backend_->revoke_sequence();
    if (status_generation_.load(std::memory_order_acquire) == generation) {
        std::uint32_t current = status_word_.load(std::memory_order_acquire);
        while (decode_state(current) == State::kAccepted ||
               decode_state(current) == State::kRunning) {
            const std::uint8_t executed = decode_executed(current);
            const std::uint32_t aborted = encode_status(
                State::kAborted, executed, true, executed,
                TerminalCode::kSequenceAborted);
            if (status_word_.compare_exchange_weak(
                    current, aborted, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
    }
    cancel_requested_.store(true, std::memory_order_release);
#ifndef HID_SEQUENCE_NATIVE_TEST
    if (s_task != nullptr) xTaskNotifyGive(s_task);
#endif
}

void Controller::abort() {
    const std::uint32_t generation =
        reserved_generation_.load(std::memory_order_acquire);
    if (generation == 0) {
        cancel_requested_.store(true, std::memory_order_release);
        return;
    }
    abort_generation(generation);
}

void Controller::retire_owner(std::uint64_t local_owner_id) {
    if (local_owner_id == 0) return;
    while (true) {
        const std::uint32_t generation =
            status_generation_.load(std::memory_order_acquire);
        if (generation == 0 ||
            status_owner_low_.load(std::memory_order_relaxed) !=
                owner_low(local_owner_id) ||
            status_owner_high_.load(std::memory_order_relaxed) !=
                owner_high(local_owner_id)) {
            return;
        }
        std::uint32_t expected = generation;
        if (status_generation_.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            abort_generation(generation);
            return;
        }
    }
}

bool Controller::active() const {
    return reserved_generation_.load(std::memory_order_acquire) != 0;
}

bool Controller::wait_local(std::uint32_t milliseconds) {
    const std::uint64_t wait_deadline = now_(now_context_) +
        static_cast<std::uint64_t>(milliseconds) * 1000U;
    const std::uint64_t execution_deadline = admission_us_ +
        static_cast<std::uint64_t>(kExecutionDeadlineMs) * 1000U;
    while (true) {
        const std::uint64_t now = now_(now_context_);
        if (cancel_requested_.load(std::memory_order_acquire) ||
            !backend_->authority_current(authority_)) return false;
        const WaitDecision decision = wait_decision(
            now, wait_deadline, execution_deadline);
        if (decision.execution_expired) return false;
        if (decision.wait_complete) return true;
#ifndef HID_SEQUENCE_NATIVE_TEST
        const std::uint32_t whole_ms =
            static_cast<std::uint32_t>(decision.block_us / 1000U);
        const TickType_t ticks = pdMS_TO_TICKS(whole_ms);
        if (ticks == 0) {
            taskYIELD();
        } else {
            (void)ulTaskNotifyTake(pdTRUE, ticks);
        }
#else
        // Native tests advance the injected monotonic clock deterministically.
#endif
    }
    return true;
}

bool Controller::execute_report(OperationKind kind, HidState *state,
                                TerminalCode *failure) {
    const std::uint64_t readiness_deadline = now_(now_context_) +
        static_cast<std::uint64_t>(kOperationReadinessMs) * 1000U;
    while (true) {
        ReportResult result = ReportResult::kNotReady;
        if (kind == OperationKind::kKeyPress || kind == OperationKind::kKeyRelease) {
            result = backend_->keyboard_report(authority_,
                                               active_local_owner_id_,
                                               state->modifiers,
                                               state->keycodes);
        } else {
            result = backend_->mouse_report(authority_, active_local_owner_id_,
                                            state->mouse_buttons);
        }
        if (result == ReportResult::kAccepted) return true;
        if (result != ReportResult::kBusy || now_(now_context_) >= readiness_deadline) {
            *failure = result == ReportResult::kBusy ? TerminalCode::kHidBusy
                : result == ReportResult::kSafetyPending ? TerminalCode::kHidSafetyPending
                : result == ReportResult::kAuthorityLost ? TerminalCode::kSessionMismatch
                : TerminalCode::kHidNotReady;
            return false;
        }
        if (!wait_local(1)) return false;
    }
}

bool Controller::claim_terminal(std::uint32_t generation, State state,
                                std::uint8_t executed,
                                std::uint8_t failed_token,
                                TerminalCode code) {
    if (status_generation_.load(std::memory_order_acquire) != generation ||
        !terminal_state(state)) {
        return false;
    }
    std::uint32_t current = status_word_.load(std::memory_order_acquire);
    while (!terminal_state(decode_state(current))) {
        if (decode_state(current) != State::kRunning) return false;
        const bool failure = state == State::kFailed || state == State::kAborted;
        const std::uint32_t terminal = encode_status(
            state, executed, failure, failed_token, code);
        if (status_word_.compare_exchange_weak(
                current, terminal, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool Controller::publish_running_progress(std::uint32_t generation,
                                          std::uint8_t executed) {
    if (status_generation_.load(std::memory_order_acquire) != generation) {
        return false;
    }
    std::uint32_t current = status_word_.load(std::memory_order_acquire);
    while (decode_state(current) == State::kRunning) {
        const std::uint32_t progress = encode_status(
            State::kRunning, executed, false, 0, TerminalCode::kNone);
        if (status_word_.compare_exchange_weak(
                current, progress, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void Controller::run(std::uint32_t generation) {
    if (status_generation_.load(std::memory_order_acquire) != generation) {
        backend_->end_sequence(authority_);
        std::uint32_t expected_generation = generation;
        (void)reserved_generation_.compare_exchange_strong(
            expected_generation, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        return;
    }
    std::uint32_t accepted = encode_status(
        State::kAccepted, 0, false, 0, TerminalCode::kNone);
    const std::uint32_t running = encode_status(
        State::kRunning, 0, false, 0, TerminalCode::kNone);
    if (!status_word_.compare_exchange_strong(
            accepted, running, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        if (status_generation_.load(std::memory_order_acquire) == generation &&
            decode_state(accepted) == State::kAborted) {
            backend_->request_safety_release();
        }
        backend_->end_sequence(authority_);
        std::uint32_t expected_generation = generation;
        (void)reserved_generation_.compare_exchange_strong(
            expected_generation, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        return;
    }
    HidState current = plan_.initial_state;
    std::uint32_t default_delay = 0;
    State final_state = State::kCompleted;
    TerminalCode final_code = TerminalCode::kNone;
    std::uint8_t failed_token = 0;
    std::uint8_t executed = 0;
    bool terminal_published = false;
    for (std::size_t index = 0; index < plan_.count; ++index) {
        const bool canceled = cancel_requested_.load(std::memory_order_acquire) ||
            canceled_generation_.load(std::memory_order_acquire) == generation;
        const bool authority_lost = !backend_->authority_current(authority_);
        if (canceled || authority_lost) {
            final_state = State::kAborted;
            final_code = canceled ? TerminalCode::kSequenceAborted
                                  : TerminalCode::kSessionMismatch;
            failed_token = static_cast<std::uint8_t>(index);
            break;
        }
        if (now_(now_context_) - admission_us_ >=
            static_cast<std::uint64_t>(kExecutionDeadlineMs) * 1000U) {
            final_state = State::kFailed;
            final_code = TerminalCode::kSequenceTimeout;
            failed_token = static_cast<std::uint8_t>(index);
            break;
        }
        const Operation operation = plan_.operations[index];
        bool complete = true;
        if (operation.kind == OperationKind::kDefaultDelay) {
            default_delay = operation.value;
        } else if (operation.kind == OperationKind::kWait) {
            complete = wait_local(operation.value);
        } else {
            (void)apply_transition(operation, &current);
            complete = execute_report(operation.kind, &current, &final_code);
            if (complete && index + 1 < plan_.count) complete = wait_local(default_delay);
        }
        if (!complete) {
            const bool canceled = cancel_requested_.load(std::memory_order_acquire) ||
                canceled_generation_.load(std::memory_order_acquire) == generation;
            const bool authority_lost = !backend_->authority_current(authority_);
            final_state = canceled || authority_lost ? State::kAborted
                                                      : State::kFailed;
            if (canceled) final_code = TerminalCode::kSequenceAborted;
            else if (authority_lost) final_code = TerminalCode::kSessionMismatch;
            else if (final_code == TerminalCode::kNone) final_code = TerminalCode::kSequenceTimeout;
            failed_token = static_cast<std::uint8_t>(index);
            break;
        }
        const bool canceled_after =
            cancel_requested_.load(std::memory_order_acquire) ||
            canceled_generation_.load(std::memory_order_acquire) == generation;
        const bool authority_lost_after =
            !backend_->authority_current(authority_);
        if (canceled_after || authority_lost_after) {
            final_state = State::kAborted;
            final_code = canceled_after ? TerminalCode::kSequenceAborted
                                        : TerminalCode::kSessionMismatch;
            failed_token = static_cast<std::uint8_t>(index);
            break;
        }
        if (now_(now_context_) - admission_us_ >=
            static_cast<std::uint64_t>(kExecutionDeadlineMs) * 1000U) {
            final_state = State::kFailed;
            final_code = TerminalCode::kSequenceTimeout;
            failed_token = static_cast<std::uint8_t>(index);
            break;
        }
        executed = static_cast<std::uint8_t>(index + 1);
        if (index + 1 == plan_.count) {
            terminal_published = claim_terminal(
                generation, State::kCompleted, executed, 0,
                TerminalCode::kNone);
            break;
        }
        if (!publish_running_progress(generation, executed)) break;
    }
    if (!terminal_published && !terminal_state(
            decode_state(status_word_.load(std::memory_order_acquire)))) {
        (void)claim_terminal(
            generation, final_state, executed, failed_token, final_code);
    }
    const State published_state =
        decode_state(status_word_.load(std::memory_order_acquire));
    if (published_state != State::kCompleted) {
        backend_->request_safety_release();
    }
    backend_->end_sequence(authority_);
    std::uint32_t expected_generation = generation;
    (void)reserved_generation_.compare_exchange_strong(
        expected_generation, 0, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

#ifndef HID_SEQUENCE_NATIVE_TEST
void Controller::task_entry(void *context) {
    Controller *controller = static_cast<Controller *>(context);
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const std::uint32_t generation =
            controller->runnable_generation_.exchange(
                0, std::memory_order_acq_rel);
        if (generation != 0) controller->run(generation);
    }
}
#else
void Controller::run_for_test() {
    const std::uint32_t generation = runnable_generation_.exchange(
        0, std::memory_order_acq_rel);
    if (generation != 0) run(generation);
}

void Controller::set_next_generation_for_test(std::uint32_t generation) {
    next_generation_.store(generation, std::memory_order_release);
}
#endif

}  // namespace hid_sequence
