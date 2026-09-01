#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hid_route/hid_route.hpp"
#include "usb_lifecycle/usb_lifecycle.hpp"

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

// Future transports are identity values only in U7.2A. No BLE adapter or
// execution path exists in this slice.
enum class HidTransport : std::uint8_t {
    kUsb,
    kBle,
};

enum class LifecycleSafetyResult : std::uint8_t {
    kClean,
    kPending,
    kUncertain,
};

struct StatusSnapshot {
    bool mounted = false;
    bool suspended = false;
    bool keyboard_ready = false;
    bool mouse_ready = false;
};

// Immutable Stage-A evidence for a USB lifecycle command.  Runtime state is
// captured before the lifecycle action can be observed by its executor.
struct UsbTransitionOutcome {
    usb_lifecycle::TransitionResult action_result = usb_lifecycle::TransitionResult::kBusy;
    bool snapshot_valid = false;
    usb_lifecycle::Snapshot lifecycle{};
    StatusSnapshot runtime{};
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

// This is deliberately separate from USB lifecycle generation. It is a lock-free
// lifecycle publication token that defines HID-control authority boundaries.
// Unsigned wrap is well-defined; practical lifecycle frequency cannot reach it.
using AuthorityEpoch = std::uint32_t;
using UsbGeneration = usb_lifecycle::Generation;
using RouteGeneration = hid_route::Generation;
using HidTicketId = std::uint32_t;

// Every queued or in-flight HID item is permanently bound to one output route
// and transport. It is never rerouted or replayed under another identity.
struct HidWorkToken {
    AuthorityEpoch authority_epoch = 0;
    RouteGeneration route_generation = 0;
    HidTransport transport = HidTransport::kUsb;
    UsbGeneration transport_generation = 0;
    HidTicketId ticket_id = 0;
    std::uint32_t release_epoch = 0;
};

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
    std::atomic<UsbGeneration> transport_generation{0};
    std::atomic<AuthorityEpoch> authority_epoch{0};
    std::atomic<RouteGeneration> route_generation{0};
    std::atomic<HidTransport> transport{HidTransport::kUsb};
    std::atomic<ReleaseAllInterfaceState> keyboard{ReleaseAllInterfaceState::kUnresolved};
    std::atomic<ReleaseAllInterfaceState> mouse{ReleaseAllInterfaceState::kUnresolved};
    std::atomic_bool active{false};
    std::atomic_bool finalized{false};
    std::atomic_bool failed_before_finalization{false};
    std::atomic_bool canceled{false};
};

struct ReleaseAllSnapshot {
    UsbGeneration transport_generation = 0;
    AuthorityEpoch authority_epoch = 0;
    RouteGeneration route_generation = 0;
    HidTransport transport = HidTransport::kUsb;
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

    // U7.1A internal-only future lifecycle boundary. It does not issue USB
    // hardware calls; U7.1B will connect it to the executor implementation.
    UsbTransitionOutcome request_usb_attach(usb_lifecycle::Executor &executor);
    UsbTransitionOutcome request_usb_detach(usb_lifecycle::Executor &executor);
    usb_lifecycle::Snapshot usb_lifecycle_snapshot() const;
    hid_route::Snapshot route_snapshot() const;

    // Project-owned, lifecycle-only safety operation. It intentionally does
    // not create a public hid.release_all cache identity. While detaching it
    // is the sole permitted old-generation HID work.
    LifecycleSafetyResult begin_lifecycle_detach_safety();
    bool lifecycle_detach_safety_clean() const;
    void mark_lifecycle_detach_uncertain(UsbGeneration old_generation);
    void on_driver_uninstalled();
    void complete_usb_install_success();
    void complete_usb_install_clean_failure(std::int32_t error_code);
    void complete_usb_install_ambiguous_failure(std::int32_t error_code);
    UsbGeneration begin_usb_uninstall();
    void complete_usb_uninstall_success();
    void complete_usb_uninstall_failure(std::int32_t error_code);
    // Called only by the shared control executor after old-route all-up work
    // received its bounded opportunity and before USB teardown advances its
    // independent generation.
    void complete_usb_detach_route_invalidation(hid_route::Snapshot old_route);

    StatusSnapshot status() const;
    UsbGeneration attach_generation() const;
    HidWorkToken in_flight_token(Interface interface) const;
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
    bool report_complete_for_token(std::uint8_t instance, HidWorkToken token,
                                   const std::uint8_t *report = nullptr,
                                   std::uint16_t length = 0);
    bool report_failed_for_token(std::uint8_t instance, HidWorkToken token,
                                 const std::uint8_t *report = nullptr,
                                 std::uint16_t length = 0);

    KeyboardState keyboard_state() const;
    MouseState mouse_state() const;
    bool safety_required(Interface interface) const;
    bool host_state_uncertain(Interface interface) const;
    bool report_in_flight(Interface interface) const;

#ifdef HID_RUNTIME_NATIVE_TEST
    using TestHook = void (*)(StateMachine *);
    void set_before_ticket_publish_hook_for_test(TestHook hook);
    void set_before_submit_hook_for_test(TestHook hook);
    void set_before_release_reconciliation_hook_for_test(TestHook hook);
#endif

  private:
    struct InterfaceState {
        std::atomic<std::uint8_t> slot_state{0};  // empty, writing, ready, executing
        UsbGeneration slot_transport_generation = 0;
        AuthorityEpoch slot_authority_epoch = 0;
        RouteGeneration slot_route_generation = 0;
        HidTransport slot_transport = HidTransport::kUsb;
        HidTicketId slot_ticket_id = 0;
        std::uint32_t slot_release_epoch = 0;
        ReportKind slot_kind = ReportKind::kUnsafeKeyboard;
        std::uint8_t slot_length = 0;
        std::uint8_t slot_report[8]{};

        std::atomic_bool in_flight{false};
        UsbGeneration in_flight_transport_generation = 0;
        AuthorityEpoch in_flight_authority_epoch = 0;
        RouteGeneration in_flight_route_generation = 0;
        HidTransport in_flight_transport = HidTransport::kUsb;
        HidTicketId in_flight_ticket_id = 0;
        std::uint32_t in_flight_release_epoch = 0;
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
    bool compatibility_usb_route_can_select() const;
    void apply_u7_1b_compatibility_route();
    bool unsafe_route_active(RouteGeneration generation, HidTransport transport) const;
    bool safety_transport_active(Interface interface) const;
    bool any_safety_required() const;
    bool release_request_is_current(UsbGeneration generation,
                                    AuthorityEpoch authority_epoch,
                                    std::uint32_t release_epoch) const;
    void reconcile_zero_work_release(UsbGeneration generation,
                                     AuthorityEpoch authority_epoch,
                                     std::uint32_t release_epoch);
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
                                 UsbGeneration current_generation,
                                 AuthorityEpoch current_authority_epoch);
    bool process_mouse_ticket(SubmitFn submit, void *context,
                              UsbGeneration current_generation,
                              AuthorityEpoch current_authority_epoch);

    struct KeyboardReportTicket {
        std::atomic<KeyboardReportTicketState> state{KeyboardReportTicketState::kFree};
        std::atomic<UsbGeneration> transport_generation{0};
        std::atomic<AuthorityEpoch> authority_epoch{0};
        std::atomic<RouteGeneration> route_generation{0};
        std::atomic<HidTransport> transport{HidTransport::kUsb};
        std::atomic<HidTicketId> ticket_id{0};
        std::atomic<std::uint32_t> release_epoch{0};
        std::atomic<KeyboardReportTicketOutcome> outcome{KeyboardReportTicketOutcome::kNone};
        std::uint8_t report[8]{};
    };

    struct MouseReportTicket {
        std::atomic<MouseReportTicketState> state{MouseReportTicketState::kFree};
        std::atomic<UsbGeneration> transport_generation{0};
        std::atomic<AuthorityEpoch> authority_epoch{0};
        std::atomic<RouteGeneration> route_generation{0};
        std::atomic<HidTransport> transport{HidTransport::kUsb};
        std::atomic<HidTicketId> ticket_id{0};
        std::atomic<std::uint32_t> release_epoch{0};
        std::atomic<MouseReportTicketOutcome> outcome{MouseReportTicketOutcome::kNone};
        std::uint8_t report[5]{};
    };

    // ESP32-S3 has native lock-free 32-bit atomics. Keep this fixed-width
    // publication token independent from attach generation so the lifecycle
    // callback never needs to wait for the UART/control task.
    static_assert(std::atomic<AuthorityEpoch>::is_always_lock_free);
    static_assert(std::atomic<UsbGeneration>::is_always_lock_free);
    static_assert(std::atomic<RouteGeneration>::is_always_lock_free);
    static_assert(std::atomic<HidTicketId>::is_always_lock_free);
    usb_lifecycle::StateMachine usb_lifecycle_{};
    hid_route::StateMachine route_{};
    std::atomic<HidTicketId> next_ticket_id_{1};
    std::atomic<AuthorityEpoch> authority_epoch_{0};
    std::atomic<std::uint32_t> release_epoch_{0};
    std::atomic<std::uint32_t> release_request_generation_{0};
    std::atomic<AuthorityEpoch> release_request_authority_epoch_{0};
    std::atomic<std::uint32_t> release_request_epoch_{0};
    std::atomic<std::uint8_t> status_bits_{0};  // mounted, suspended, kbd-ready, mouse-ready
    std::atomic_bool release_requested_{false};
    InterfaceState interfaces_[2]{};
    ReleaseAllTicket release_ticket_{};
    KeyboardReportTicket keyboard_ticket_{};
    MouseReportTicket mouse_ticket_{};
#ifdef HID_RUNTIME_NATIVE_TEST
    TestHook before_ticket_publish_hook_ = nullptr;
    TestHook before_submit_hook_ = nullptr;
    TestHook before_release_reconciliation_hook_ = nullptr;
#endif
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
    LifecycleSafetyResult run_lifecycle_detach_safety();
    void on_driver_uninstalled();
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
    void notify_lifecycle_safety_waiter();

    StateMachine state_machine_;
    std::atomic<std::uint8_t> result_bits_{0};  // sent bits 0/1, failed bits 2/3
    std::atomic<void *> lifecycle_safety_waiter_{nullptr};
};

}  // namespace hid_runtime
