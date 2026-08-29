#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hid_runtime {

enum class Interface : std::uint8_t {
    kKeyboard = 0,
    kMouse = 1,
};

enum class ReportKind : std::uint8_t {
    kUnsafeKeyboard,
    kUnsafeMouse,
    kSafetyKeyboard,
    kSafetyMouse,
};

struct StatusSnapshot {
    bool mounted = false;
    bool suspended = false;
    bool keyboard_ready = false;
    bool mouse_ready = false;
};

struct KeyboardState {
    std::uint8_t modifiers = 0;
    std::array<std::uint8_t, 6> keycodes{};
};

struct MouseState {
    std::uint8_t buttons = 0;
};

enum class KeyboardReportFailure : std::uint8_t {
    kNone,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

enum class KeyboardReportBeginResult : std::uint8_t {
    kAlreadySet,
    kPublished,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

enum class KeyboardReportState : std::uint8_t {
    kAlreadySet,
    kSubmitted,
};

enum class KeyboardReportTicketState : std::uint8_t {
    kFree,
    kWriting,
    kPublished,
    kClaimed,
    kSubmitted,
    kNotReady,
    kCanceled,
};

enum class KeyboardReportTicketOutcome : std::uint8_t {
    kNone,
    kSubmitted,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

struct KeyboardReportSnapshot {
    KeyboardReportTicketState state = KeyboardReportTicketState::kFree;
    KeyboardReportTicketOutcome outcome = KeyboardReportTicketOutcome::kNone;
};

struct KeyboardReportResult {
    bool success = false;
    bool authority_lost = false;
    KeyboardReportState state = KeyboardReportState::kSubmitted;
    KeyboardReportFailure failure = KeyboardReportFailure::kNotReady;
};

enum class MouseReportFailure : std::uint8_t {
    kNone,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

enum class MouseReportBeginResult : std::uint8_t {
    kAlreadySet,
    kPublished,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

enum class MouseReportState : std::uint8_t {
    kAlreadySet,
    kSubmitted,
};

enum class MouseReportTicketState : std::uint8_t {
    kFree,
    kWriting,
    kPublished,
    kClaimed,
    kSubmitted,
    kNotReady,
    kCanceled,
};

enum class MouseReportTicketOutcome : std::uint8_t {
    kNone,
    kSubmitted,
    kNotReady,
    kBusy,
    kSafetyPending,
    kAuthorityLost,
};

struct MouseReportSnapshot {
    MouseReportTicketState state = MouseReportTicketState::kFree;
    MouseReportTicketOutcome outcome = MouseReportTicketOutcome::kNone;
};

struct MouseReportResult {
    bool success = false;
    bool authority_lost = false;
    MouseReportState state = MouseReportState::kSubmitted;
    MouseReportFailure failure = MouseReportFailure::kNotReady;
};

// This is deliberately separate from attach_generation.  It is a lock-free
// lifecycle publication token that defines HID-control authority boundaries.
// Unsigned wrap is well-defined; practical lifecycle frequency cannot reach it.
using AuthorityEpoch = std::uint32_t;

using SubmitFn = bool (*)(void *context, std::uint8_t instance,
                          const std::uint8_t *report, std::uint16_t length);

enum class ReleaseAllInterfaceState : std::uint8_t {
    kUnresolved,
    kAlreadyUp,
    kSubmitted,
    kPending,
    kCanceled,
};

// Fixed-size, heap-free outcome bridge between the UART/control task and the
// TinyUSB SOF executor. Interface outcomes are historical: kSubmitted means
// tud_hid_n_report() accepted the all-up report, not that the host completed it.
struct ReleaseAllTicket {
    std::atomic<std::uint32_t> attach_generation{0};
    std::atomic<AuthorityEpoch> authority_epoch{0};
    std::atomic<ReleaseAllInterfaceState> keyboard{ReleaseAllInterfaceState::kUnresolved};
    std::atomic<ReleaseAllInterfaceState> mouse{ReleaseAllInterfaceState::kUnresolved};
    std::atomic_bool active{false};
    std::atomic_bool finalized{false};
    std::atomic_bool failed_before_finalization{false};
    std::atomic_bool canceled{false};
};

struct ReleaseAllSnapshot {
    std::uint32_t attach_generation = 0;
    AuthorityEpoch authority_epoch = 0;
    ReleaseAllInterfaceState keyboard = ReleaseAllInterfaceState::kUnresolved;
    ReleaseAllInterfaceState mouse = ReleaseAllInterfaceState::kUnresolved;
    bool active = false;
    bool finalized = false;
    bool failed_before_finalization = false;
    bool canceled = false;
};

struct ReleaseAllResult {
    bool success = false;
    bool authority_lost = false;
    ReleaseAllInterfaceState keyboard = ReleaseAllInterfaceState::kPending;
    ReleaseAllInterfaceState mouse = ReleaseAllInterfaceState::kPending;
};

// TinyUSB-independent state and mailbox core. Producers may run from the
// control/application task; execute() is called only from the TinyUSB task.
// The fixed one-slot-per-interface mailbox never allocates or blocks.
class StateMachine {
  public:
    StateMachine();

    void on_mount();
    void on_unmount();
    void on_suspend();
    void on_resume();
    void set_ready(Interface interface, bool ready);

    StatusSnapshot status() const;
    std::uint32_t attach_generation() const;
    AuthorityEpoch authority_epoch() const;

    // Unsafe reports are accepted only while mounted, ready, and safety-clear.
    // A rejected report is discarded and is never replayed later.
    bool queue_keyboard_report(std::uint8_t modifiers,
                               const std::array<std::uint8_t, 6> &keycodes);
    bool queue_mouse_report(std::uint8_t buttons, std::int8_t x, std::int8_t y,
                            std::int8_t vertical, std::int8_t horizontal);

    // Public keyboard reports use a dedicated fixed-size ticket.  The
    // control task publishes the payload and the TinyUSB SOF executor claims
    // and resolves it without ever reading a partially-written report.
    KeyboardReportBeginResult begin_keyboard_report(
        std::uint8_t modifiers, const std::array<std::uint8_t, 6> &keycodes);
    KeyboardReportSnapshot keyboard_report_snapshot() const;
    bool cancel_keyboard_report();
    void finalize_keyboard_report();

    // Public mouse reports use a dedicated fixed-size ticket. Relative axes
    // are kept only in the ticket/in-flight identity; confirmed state stores
    // the persistent button bitmap alone.
    MouseReportBeginResult begin_mouse_report(std::uint8_t buttons,
                                              std::int8_t x, std::int8_t y,
                                              std::int8_t vertical,
                                              std::int8_t horizontal);
    MouseReportSnapshot mouse_report_snapshot() const;
    bool cancel_mouse_report();
    void finalize_mouse_report();

    // Internal safety primitive. It may be called repeatedly; only interfaces
    // with held or uncertain host state require an all-up report.
    void request_release_all();
    void begin_release_all();
    ReleaseAllSnapshot release_all_snapshot() const;
    void finalize_release_all();
    void cancel_queued(Interface interface);

    // TinyUSB-task executor and completion notifications.
    void execute(SubmitFn submit, void *context);
    bool report_complete(std::uint8_t instance,
                         const std::uint8_t *report = nullptr,
                         std::uint16_t length = 0);
    bool report_failed(std::uint8_t instance,
                       const std::uint8_t *report = nullptr,
                       std::uint16_t length = 0);

    KeyboardState keyboard_state() const;
    MouseState mouse_state() const;
    bool safety_required(Interface interface) const;
    bool host_state_uncertain(Interface interface) const;
    bool report_in_flight(Interface interface) const;

  private:
    struct InterfaceState {
        std::atomic<std::uint8_t> slot_state{0};  // empty, writing, ready, executing
        std::uint32_t slot_generation = 0;
        AuthorityEpoch slot_authority_epoch = 0;
        std::uint32_t slot_release_epoch = 0;
        ReportKind slot_kind = ReportKind::kUnsafeKeyboard;
        std::uint8_t slot_length = 0;
        std::uint8_t slot_report[8]{};

        std::atomic_bool in_flight{false};
        std::uint32_t in_flight_generation = 0;
        AuthorityEpoch in_flight_authority_epoch = 0;
        ReportKind in_flight_kind = ReportKind::kUnsafeKeyboard;
        std::uint8_t in_flight_length = 0;
        std::uint8_t in_flight_report[8]{};
        std::atomic_bool safety_required{false};
        std::atomic_bool host_state_uncertain{false};
        // A lock-free summary used by the control task when deciding whether
        // an interface is already known all-up. The detailed report structs
        // remain executor-owned and are not read cross-task.
        std::atomic_bool logical_state_held{false};
        // Seqlock-protected confirmed keyboard Boot report.  The detailed
        // KeyboardState remains executor-owned; producers only consume this
        // immutable snapshot when deciding whether a new request is
        // already_set.
        std::atomic<std::uint32_t> confirmed_sequence{0};
        std::atomic<std::uint32_t> confirmed_low{0};
        std::atomic<std::uint32_t> confirmed_high{0};
        std::atomic<std::uint8_t> confirmed_mouse_buttons{0};
        KeyboardState keyboard{};
        MouseState mouse{};
    };

    static constexpr std::uint8_t kSlotEmpty = 0;
    static constexpr std::uint8_t kSlotWriting = 1;
    static constexpr std::uint8_t kSlotReady = 2;
    static constexpr std::uint8_t kSlotExecuting = 3;
    static constexpr std::uint8_t kSlotCanceled = 4;

    InterfaceState &state(Interface interface);
    const InterfaceState &state(Interface interface) const;
    bool mounted_and_active(Interface interface) const;
    bool any_safety_required() const;
    bool queue_safety(Interface interface);
    bool queue_report(Interface interface, ReportKind kind,
                      const std::uint8_t *report, std::uint8_t length);
    void clear_interface(InterfaceState &interface_state);
    void preserve_suspend_safety(InterfaceState &interface_state);
    void cancel_release_ticket();
    void cancel_keyboard_ticket(KeyboardReportTicketOutcome outcome);
    void cancel_mouse_ticket(MouseReportTicketOutcome outcome);
    bool known_all_up(Interface interface) const;
    void set_release_outcome(Interface interface, ReleaseAllInterfaceState outcome);
    void write_confirmed_keyboard(const std::uint8_t *report);
    std::array<std::uint8_t, 8> read_confirmed_keyboard() const;
    bool confirmed_keyboard_equals(const std::uint8_t *report) const;
    void write_confirmed_mouse(std::uint8_t buttons);
    std::uint8_t read_confirmed_mouse() const;
    bool process_keyboard_ticket(SubmitFn submit, void *context,
                                 std::uint32_t current_generation,
                                 AuthorityEpoch current_authority_epoch);
    bool process_mouse_ticket(SubmitFn submit, void *context,
                              std::uint32_t current_generation,
                              AuthorityEpoch current_authority_epoch);

    struct KeyboardReportTicket {
        std::atomic<KeyboardReportTicketState> state{KeyboardReportTicketState::kFree};
        std::atomic<std::uint32_t> attach_generation{0};
        std::atomic<AuthorityEpoch> authority_epoch{0};
        std::atomic<std::uint32_t> release_epoch{0};
        std::atomic<KeyboardReportTicketOutcome> outcome{KeyboardReportTicketOutcome::kNone};
        std::uint8_t report[8]{};
    };

    struct MouseReportTicket {
        std::atomic<MouseReportTicketState> state{MouseReportTicketState::kFree};
        std::atomic<std::uint32_t> attach_generation{0};
        std::atomic<AuthorityEpoch> authority_epoch{0};
        std::atomic<std::uint32_t> release_epoch{0};
        std::atomic<MouseReportTicketOutcome> outcome{MouseReportTicketOutcome::kNone};
        std::uint8_t report[5]{};
    };

    // ESP32-S3 has native lock-free 32-bit atomics. Keep this fixed-width
    // publication token independent from attach generation so the lifecycle
    // callback never needs to wait for the UART/control task.
    static_assert(std::atomic<AuthorityEpoch>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<AuthorityEpoch> authority_epoch_{0};
    std::atomic<std::uint32_t> release_epoch_{0};
    std::atomic<std::uint32_t> release_request_generation_{0};
    std::atomic<AuthorityEpoch> release_request_authority_epoch_{0};
    std::atomic<std::uint8_t> status_bits_{0};  // mounted, suspended, kbd-ready, mouse-ready
    std::atomic_bool release_requested_{false};
    InterfaceState interfaces_[2]{};
    ReleaseAllTicket release_ticket_{};
    KeyboardReportTicket keyboard_ticket_{};
    MouseReportTicket mouse_ticket_{};
};

// Hardware adapter. All tud_hid_* calls are confined to service_sof(), which
// is invoked by TinyUSB's public SOF callback in TinyUSB task context.
class Runtime {
  public:
    void initialize();
    void on_mount();
    void on_unmount();
    void on_suspend();
    void on_resume();
    StatusSnapshot status_snapshot() const;
    AuthorityEpoch authority_epoch() const;

    bool queue_keyboard_report(std::uint8_t modifiers,
                               const std::array<std::uint8_t, 6> &keycodes);
    bool queue_mouse_report(std::uint8_t buttons, std::int8_t x, std::int8_t y,
                            std::int8_t vertical, std::int8_t horizontal);
    KeyboardReportResult keyboard_report(
        std::uint8_t modifiers, const std::array<std::uint8_t, 6> &keycodes);
    MouseReportResult mouse_report(std::uint8_t buttons, std::int8_t x, std::int8_t y,
                                   std::int8_t vertical, std::int8_t horizontal);
    void request_release_all();
    ReleaseAllResult release_all();
    void service_sof();
    void on_report_complete(std::uint8_t instance,
                            const std::uint8_t *report = nullptr,
                            std::uint16_t length = 0);
    bool on_report_failed(std::uint8_t instance,
                          const std::uint8_t *report = nullptr,
                          std::uint16_t length = 0);

    // Results are consumed by the application task for bounded diagnostic
    // logging; the TinyUSB callback itself never logs or blocks.
    bool take_report_sent(Interface interface);
    bool take_report_failed(Interface interface);

    StateMachine &state_machine() { return state_machine_; }

  private:
    static bool submit_report(void *, std::uint8_t instance,
                              const std::uint8_t *report, std::uint16_t length);
    void set_result(Interface interface, bool failed);

    StateMachine state_machine_;
    std::atomic<std::uint8_t> result_bits_{0};  // sent bits 0/1, failed bits 2/3
};

}  // namespace hid_runtime
