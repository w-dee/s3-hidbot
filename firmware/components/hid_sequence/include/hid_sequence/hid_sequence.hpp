#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hid_capability/hid_capability.hpp"

namespace hid_sequence {

inline constexpr std::size_t kMaximumCodeBytes = 320;
inline constexpr std::size_t kMaximumTokens = 64;
inline constexpr std::uint32_t kMaximumDefaultDelayMs = 1000;
inline constexpr std::uint32_t kMaximumWaitMs = 2000;
inline constexpr std::uint32_t kMaximumScheduledDurationMs = 2500;
inline constexpr std::uint32_t kExecutionDeadlineMs = 4000;
inline constexpr std::uint32_t kOperationReadinessMs = 100;

struct HidState {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> keycodes{};
    std::uint8_t mouse_buttons = 0;
};

struct ExecutionAuthority {
    std::uint32_t generation = 0;
    std::uint32_t authority_epoch = 0;
    std::uint32_t release_epoch = 0;
    std::uint32_t profile_activation_epoch = 0;
    hid_capability::ReportMask active_roles = 0;
    std::uint8_t transport = 0;
    std::uint32_t route_generation = 0;
};

enum class OperationKind : std::uint8_t {
    kDefaultDelay,
    kWait,
    kKeyPress,
    kKeyRelease,
    kMousePress,
    kMouseRelease,
};

struct Operation {
    OperationKind kind = OperationKind::kWait;
    std::uint16_t value = 0;
};

struct Plan {
    std::array<Operation, kMaximumTokens> operations{};
    std::size_t count = 0;
    std::uint32_t scheduled_duration_ms = 0;
    HidState initial_state{};
    hid_capability::ReportMask required_roles = 0;
};

bool parse(std::string_view code, const HidState &initial_state, Plan *plan);

struct WaitDecision {
    bool wait_complete = false;
    bool execution_expired = false;
    std::uint64_t block_us = 0;
};

WaitDecision wait_decision(std::uint64_t now, std::uint64_t wait_deadline,
                           std::uint64_t execution_deadline);

enum class AdmissionResult : std::uint8_t {
    kAccepted,
    kInvalid,
    kBusy,
    kNotReady,
    kSafetyPending,
    kUnsupportedOperation,
};

enum class ReportResult : std::uint8_t {
    kAccepted,
    kBusy,
    kNotReady,
    kSafetyPending,
    kAuthorityLost,
};

enum class State : std::uint8_t {
    kAccepted,
    kRunning,
    kCompleted,
    kFailed,
    kAborted,
};

enum class TerminalCode : std::uint8_t {
    kNone,
    kHidBusy,
    kHidNotReady,
    kHidSafetyPending,
    kSessionMismatch,
    kSequenceTimeout,
    kSequenceAborted,
};

struct Status {
    std::int32_t sequence_id = 0;
    State state = State::kAccepted;
    bool started = false;
    std::uint8_t executed = 0;
    bool failed_token_present = false;
    std::uint8_t failed_token = 0;
    TerminalCode code = TerminalCode::kNone;
};

class Backend {
  public:
    virtual ~Backend() = default;
    virtual AdmissionResult begin_sequence(HidState *initial_state,
                                           ExecutionAuthority *authority) = 0;
    virtual bool authority_current(ExecutionAuthority authority) const = 0;
    virtual void end_sequence(ExecutionAuthority authority) = 0;
    virtual void revoke_sequence() = 0;
    virtual ReportResult keyboard_report(
        ExecutionAuthority authority,
        std::uint64_t originating_local_owner_id,
        std::uint8_t modifiers,
        const std::array<std::uint8_t, 6> &keycodes) = 0;
    virtual ReportResult mouse_report(ExecutionAuthority authority,
                                      std::uint64_t originating_local_owner_id,
                                      std::uint8_t buttons) = 0;
    virtual void request_safety_release() = 0;
};

using NowFn = std::uint64_t (*)(void *context);

class Controller {
  public:
    bool initialize(Backend *backend, NowFn now, void *now_context);
    AdmissionResult start(std::uint64_t local_owner_id,
                          std::int32_t sequence_id,
                          std::string_view code);
    bool status(std::uint64_t local_owner_id,
                std::int32_t sequence_id,
                Status *status);
    void abort();
    // True means there was no unsafe independent terminal outcome, or this
    // call atomically won the exact abort/cleanup ownership transition.
    bool abort_for_release();
    void retire_owner(std::uint64_t local_owner_id);
    bool active() const;

#ifdef HID_SEQUENCE_NATIVE_TEST
    using TestHook = void (*)(Controller *);
    AdmissionResult start(std::int32_t sequence_id, std::string_view code) {
        return start(1, sequence_id, code);
    }
    bool status(std::int32_t sequence_id, Status *status) {
        return this->status(1, sequence_id, status);
    }
    void run_for_test();
    void set_next_generation_for_test(std::uint32_t generation);
    void set_before_cleanup_decision_hook_for_test(TestHook hook);
    void set_before_safety_cleanup_hook_for_test(TestHook hook);
#endif

  private:
    bool wait_local(std::uint32_t milliseconds);
    bool execute_report(OperationKind kind, HidState *state,
                        TerminalCode *failure);
    bool claim_terminal(std::uint32_t generation, State state,
                        std::uint8_t executed, std::uint8_t failed_token,
                        TerminalCode code);
    bool publish_running_progress(std::uint32_t generation,
                                  std::uint8_t executed);
    void abort_generation(std::uint32_t generation);
    void run(std::uint32_t generation);
#ifndef HID_SEQUENCE_NATIVE_TEST
    static void task_entry(void *context);
#endif

    Backend *backend_ = nullptr;
    NowFn now_ = nullptr;
    void *now_context_ = nullptr;
    Plan plan_{};
    ExecutionAuthority authority_{};
    std::uint64_t active_local_owner_id_ = 0;
    std::uint64_t admission_us_ = 0;
    std::atomic<std::uint32_t> next_generation_{1};
    std::atomic<std::uint32_t> reserved_generation_{0};
    std::atomic<std::uint32_t> runnable_generation_{0};
    std::atomic<std::uint32_t> canceled_generation_{0};
    std::atomic<std::uint32_t> status_owner_low_{0};
    std::atomic<std::uint32_t> status_owner_high_{0};
    std::atomic<std::uint32_t> status_generation_{0};
    std::atomic<std::uint32_t> status_word_{0};
    std::atomic_bool initialized_{false};
    std::atomic_bool cancel_requested_{false};
    std::atomic<std::int32_t> sequence_id_{0};
#ifdef HID_SEQUENCE_NATIVE_TEST
    TestHook before_cleanup_decision_hook_ = nullptr;
    TestHook before_safety_cleanup_hook_ = nullptr;
#endif
};

const char *state_name(State state);
const char *terminal_code_name(TerminalCode code);

}  // namespace hid_sequence
