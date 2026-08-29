#include "hid_runtime/hid_runtime.hpp"

#include <cstring>

#ifndef HID_RUNTIME_NATIVE_TEST
#include "class/hid/hid_device.h"
#include "device/usbd.h"
#endif

namespace hid_runtime {
namespace {

constexpr std::uint8_t kMountedBit = 1U << 0;
constexpr std::uint8_t kSuspendedBit = 1U << 1;
constexpr std::uint8_t kKeyboardReadyBit = 1U << 2;
constexpr std::uint8_t kMouseReadyBit = 1U << 3;

std::size_t index(Interface interface) {
    return static_cast<std::size_t>(interface);
}

bool unsafe_report_holds_state(ReportKind kind, const std::uint8_t *report,
                               std::uint8_t length) {
    if (report == nullptr) {
        return false;
    }
    if (kind == ReportKind::kUnsafeKeyboard) {
        if (length < 8) {
            return true;
        }
        if (report[0] != 0) {
            return true;
        }
        for (std::size_t key_index = 2; key_index < 8; ++key_index) {
            if (report[key_index] != 0) {
                return true;
            }
        }
        return false;
    }
    if (kind == ReportKind::kUnsafeMouse) {
        return length < 5 || (report[0] & 0x1fU) != 0;
    }
    return false;
}

}  // namespace

StateMachine::StateMachine() = default;

StateMachine::InterfaceState &StateMachine::state(Interface interface) {
    return interfaces_[index(interface)];
}

const StateMachine::InterfaceState &StateMachine::state(Interface interface) const {
    return interfaces_[index(interface)];
}

void StateMachine::clear_interface(InterfaceState &interface_state) {
    // Only the atomic state markers are shared with producers.  Leave the
    // fixed payload/metadata bytes untouched here so an application producer
    // that is in WRITING cannot race lifecycle cleanup; EMPTY plus the new
    // generation makes every stale payload unreachable.
    interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
    interface_state.in_flight.store(false, std::memory_order_release);
    interface_state.safety_required.store(false, std::memory_order_release);
    interface_state.host_state_uncertain.store(false, std::memory_order_release);
    interface_state.keyboard = {};
    interface_state.mouse = {};
}

void StateMachine::preserve_suspend_safety(InterfaceState &interface_state) {
    const bool keyboard_held =
        interface_state.keyboard.modifiers != 0 ||
        interface_state.keyboard.keycodes != std::array<std::uint8_t, 6>{};
    const bool mouse_held = interface_state.mouse.buttons != 0;
    const std::uint8_t slot_state = interface_state.slot_state.load(std::memory_order_acquire);
    const bool queued_unsafe_holds_state =
        slot_state == kSlotReady &&
        interface_state.slot_kind != ReportKind::kSafetyKeyboard &&
        interface_state.slot_kind != ReportKind::kSafetyMouse &&
        unsafe_report_holds_state(interface_state.slot_kind,
                                  interface_state.slot_report,
                                  interface_state.slot_length);
    const bool in_flight = interface_state.in_flight.load(std::memory_order_acquire);

    // A completed all-up is the only proof that the host knows an interface is
    // released. Suspend can race a completion, so preserve that requirement.
    if (keyboard_held || mouse_held || queued_unsafe_holds_state || in_flight ||
        interface_state.host_state_uncertain.load(std::memory_order_acquire) ||
        interface_state.safety_required.load(std::memory_order_acquire)) {
        interface_state.safety_required.store(true, std::memory_order_release);
    }
    if (in_flight) {
        interface_state.host_state_uncertain.store(true, std::memory_order_release);
        // A later completion/failure belongs to the retired authority epoch.
        interface_state.in_flight.store(false, std::memory_order_release);
    }
    std::uint8_t expected = kSlotReady;
    interface_state.slot_state.compare_exchange_strong(
        expected, kSlotCanceled, std::memory_order_acq_rel, std::memory_order_acquire);
}

void StateMachine::on_mount() {
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    clear_interface(interfaces_[0]);
    clear_interface(interfaces_[1]);
    release_request_generation_.store(0, std::memory_order_release);
    release_request_authority_epoch_.store(0, std::memory_order_release);
    release_requested_.store(false, std::memory_order_release);
    status_bits_.store(kMountedBit, std::memory_order_release);
}

void StateMachine::on_unmount() {
    // Invalidate first: work from an older attach or authority epoch can never
    // be accepted by a later executor pass, even if it races this callback.
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    status_bits_.store(0, std::memory_order_release);
    clear_interface(interfaces_[0]);
    clear_interface(interfaces_[1]);
    release_request_generation_.store(0, std::memory_order_release);
    release_request_authority_epoch_.store(0, std::memory_order_release);
    release_requested_.store(false, std::memory_order_release);
}

void StateMachine::on_suspend() {
    // This is the control-authority linearization boundary. It intentionally
    // precedes the UART task's eventual session/cache cleanup notification.
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    preserve_suspend_safety(interfaces_[0]);
    preserve_suspend_safety(interfaces_[1]);
    std::uint8_t current = status_bits_.load(std::memory_order_acquire);
    do {
        const std::uint8_t desired = static_cast<std::uint8_t>(
            (current | kSuspendedBit) &
            static_cast<std::uint8_t>(~(kKeyboardReadyBit | kMouseReadyBit)));
        if (status_bits_.compare_exchange_weak(current, desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return;
        }
    } while (true);
}

void StateMachine::on_resume() {
    // A session established during suspend is diagnostic-only; resume must
    // never silently restore it as HID-control authority.
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    status_bits_.fetch_and(static_cast<std::uint8_t>(~kSuspendedBit), std::memory_order_acq_rel);
}

void StateMachine::set_ready(Interface interface, bool ready) {
    const std::uint8_t bit = interface == Interface::kKeyboard ? kKeyboardReadyBit : kMouseReadyBit;
    if (ready && (status_bits_.load(std::memory_order_acquire) & kMountedBit) != 0 &&
        (status_bits_.load(std::memory_order_acquire) & kSuspendedBit) == 0) {
        status_bits_.fetch_or(bit, std::memory_order_release);
    } else {
        status_bits_.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_release);
    }
}

StatusSnapshot StateMachine::status() const {
    const std::uint8_t bits = status_bits_.load(std::memory_order_acquire);
    return StatusSnapshot{
        .mounted = (bits & kMountedBit) != 0,
        .suspended = (bits & kSuspendedBit) != 0,
        .keyboard_ready = (bits & kKeyboardReadyBit) != 0,
        .mouse_ready = (bits & kMouseReadyBit) != 0,
    };
}

std::uint32_t StateMachine::attach_generation() const {
    return generation_.load(std::memory_order_acquire);
}

AuthorityEpoch StateMachine::authority_epoch() const {
    return authority_epoch_.load(std::memory_order_acquire);
}

bool StateMachine::mounted_and_active(Interface interface) const {
    const StatusSnapshot snapshot = status();
    return snapshot.mounted && !snapshot.suspended &&
           (interface == Interface::kKeyboard ? snapshot.keyboard_ready : snapshot.mouse_ready);
}

bool StateMachine::any_safety_required() const {
    return interfaces_[0].safety_required.load(std::memory_order_acquire) ||
           interfaces_[1].safety_required.load(std::memory_order_acquire);
}

bool StateMachine::queue_report(Interface interface, ReportKind kind,
                                const std::uint8_t *report, std::uint8_t length) {
    if (report == nullptr || length == 0 || length > 8 ||
        !mounted_and_active(interface)) {
        return false;
    }
    const std::uint32_t queue_generation = attach_generation();
    const AuthorityEpoch queue_authority_epoch = authority_epoch();
    InterfaceState &interface_state = state(interface);
    const std::uint32_t release_epoch = release_epoch_.load(std::memory_order_acquire);
    if (kind != ReportKind::kSafetyKeyboard && kind != ReportKind::kSafetyMouse &&
        (release_requested_.load(std::memory_order_acquire) || any_safety_required())) {
        return false;
    }
    if (interface_state.safety_required.load(std::memory_order_acquire) ||
        interface_state.in_flight.load(std::memory_order_acquire)) {
        return false;
    }
    std::uint8_t expected = kSlotEmpty;
    if (!interface_state.slot_state.compare_exchange_strong(
            expected, kSlotWriting, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    interface_state.slot_generation = queue_generation;
    interface_state.slot_authority_epoch = queue_authority_epoch;
    interface_state.slot_release_epoch = release_epoch;
    interface_state.slot_kind = kind;
    interface_state.slot_length = length;
    std::memcpy(interface_state.slot_report, report, length);
    if (release_requested_.load(std::memory_order_acquire) ||
        release_epoch_.load(std::memory_order_acquire) != release_epoch ||
        attach_generation() != queue_generation ||
        authority_epoch() != queue_authority_epoch ||
        !mounted_and_active(interface)) {
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        return false;
    }
    interface_state.slot_state.store(kSlotReady, std::memory_order_release);
    return true;
}

bool StateMachine::queue_keyboard_report(std::uint8_t modifiers,
                                         const std::array<std::uint8_t, 6> &keycodes) {
    std::uint8_t report[8] = {modifiers, 0, keycodes[0], keycodes[1], keycodes[2],
                              keycodes[3], keycodes[4], keycodes[5]};
    return queue_report(Interface::kKeyboard, ReportKind::kUnsafeKeyboard, report,
                        sizeof(report));
}

bool StateMachine::queue_mouse_report(std::uint8_t buttons, std::int8_t x, std::int8_t y,
                                      std::int8_t vertical, std::int8_t horizontal) {
    const std::uint8_t report[5] = {
        static_cast<std::uint8_t>(buttons & 0x1fU),
        static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
        static_cast<std::uint8_t>(vertical), static_cast<std::uint8_t>(horizontal),
    };
    return queue_report(Interface::kMouse, ReportKind::kUnsafeMouse, report, sizeof(report));
}

bool StateMachine::queue_safety(Interface interface) {
    InterfaceState &interface_state = state(interface);
    if (!interface_state.safety_required.load(std::memory_order_acquire) ||
        interface_state.in_flight.load(std::memory_order_acquire)) {
        return false;
    }
    std::uint8_t expected = kSlotEmpty;
    if (!interface_state.slot_state.compare_exchange_strong(
            expected, kSlotWriting, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    const std::uint32_t queue_generation = attach_generation();
    const AuthorityEpoch queue_authority_epoch = authority_epoch();
    interface_state.slot_generation = queue_generation;
    interface_state.slot_authority_epoch = queue_authority_epoch;
    interface_state.slot_release_epoch = release_epoch_.load(std::memory_order_acquire);
    interface_state.slot_kind = interface == Interface::kKeyboard
                                    ? ReportKind::kSafetyKeyboard
                                    : ReportKind::kSafetyMouse;
    if (interface == Interface::kKeyboard) {
        const std::uint8_t report[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        interface_state.slot_length = sizeof(report);
        std::memcpy(interface_state.slot_report, report, sizeof(report));
    } else {
        const std::uint8_t report[5] = {0, 0, 0, 0, 0};
        interface_state.slot_length = sizeof(report);
        std::memcpy(interface_state.slot_report, report, sizeof(report));
    }
    if (attach_generation() != queue_generation ||
        authority_epoch() != queue_authority_epoch ||
        !mounted_and_active(interface)) {
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        return false;
    }
    interface_state.slot_state.store(kSlotReady, std::memory_order_release);
    return true;
}

void StateMachine::request_release_all() {
    // Producers only publish a request.  Logical state, mailbox contents, and
    // safety decisions are owned by the TinyUSB executor task.
    release_request_generation_.store(attach_generation(), std::memory_order_release);
    release_request_authority_epoch_.store(authority_epoch(), std::memory_order_release);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_requested_.store(true, std::memory_order_release);
}

void StateMachine::cancel_queued(Interface interface) {
    InterfaceState &interface_state = state(interface);
    std::uint8_t expected = kSlotReady;
    interface_state.slot_state.compare_exchange_strong(
        expected, kSlotCanceled, std::memory_order_acq_rel, std::memory_order_acquire);
}

void StateMachine::execute(SubmitFn submit, void *context) {
    if (submit == nullptr) {
        return;
    }
    const StatusSnapshot snapshot = status();
    if (!snapshot.mounted || snapshot.suspended) {
        return;
    }
    const std::uint32_t current_generation = attach_generation();
    const AuthorityEpoch current_authority_epoch = authority_epoch();
    const std::uint32_t request_generation =
        release_request_generation_.load(std::memory_order_acquire);
    const AuthorityEpoch request_authority_epoch =
        release_request_authority_epoch_.load(std::memory_order_acquire);
    const bool release_requested =
        release_requested_.exchange(false, std::memory_order_acq_rel);
    const bool release_requested_for_current_attach =
        release_requested && request_generation == current_generation &&
        request_authority_epoch == current_authority_epoch;
    for (const Interface interface : {Interface::kKeyboard, Interface::kMouse}) {
        InterfaceState &interface_state = state(interface);
        if (release_requested_for_current_attach) {
            const bool held = interface == Interface::kKeyboard
                                  ? interface_state.keyboard.modifiers != 0 ||
                                        interface_state.keyboard.keycodes != std::array<std::uint8_t, 6>{}
                                  : interface_state.mouse.buttons != 0;
            const std::uint8_t queued_state =
                interface_state.slot_state.load(std::memory_order_acquire);
            const bool queued_unsafe_holds_state =
                queued_state == kSlotReady &&
                interface_state.slot_kind != ReportKind::kSafetyKeyboard &&
                interface_state.slot_kind != ReportKind::kSafetyMouse &&
                unsafe_report_holds_state(interface_state.slot_kind,
                                          interface_state.slot_report,
                                          interface_state.slot_length);
            const bool in_flight_unsafe_holds_state =
                interface_state.in_flight.load(std::memory_order_acquire) &&
                interface_state.in_flight_kind != ReportKind::kSafetyKeyboard &&
                interface_state.in_flight_kind != ReportKind::kSafetyMouse &&
                unsafe_report_holds_state(interface_state.in_flight_kind,
                                          interface_state.in_flight_report,
                                          interface_state.in_flight_length);
            if (held || interface_state.host_state_uncertain.load(std::memory_order_acquire) ||
                interface_state.safety_required.load(std::memory_order_acquire) ||
                queued_unsafe_holds_state || in_flight_unsafe_holds_state) {
                interface_state.safety_required.store(true, std::memory_order_release);
            }
        }
        if (interface_state.slot_state.load(std::memory_order_acquire) == kSlotCanceled) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            continue;
        }
        if (!interface_state.in_flight.load(std::memory_order_acquire) &&
            interface_state.safety_required.load(std::memory_order_acquire) &&
            (interface_state.slot_state.load(std::memory_order_acquire) == kSlotEmpty)) {
            queue_safety(interface);
        }
        std::uint8_t expected = kSlotReady;
        if (!interface_state.slot_state.compare_exchange_strong(
                expected, kSlotExecuting, std::memory_order_acq_rel, std::memory_order_acquire)) {
            continue;
        }
        const ReportKind kind = interface_state.slot_kind;
        const std::uint32_t slot_generation = interface_state.slot_generation;
        const AuthorityEpoch slot_authority_epoch = interface_state.slot_authority_epoch;
        const std::uint32_t slot_release_epoch = interface_state.slot_release_epoch;
        const std::uint8_t length = interface_state.slot_length;
        const bool safety_kind = kind == ReportKind::kSafetyKeyboard ||
                                 kind == ReportKind::kSafetyMouse;
        const bool stale_unsafe =
            !safety_kind &&
            slot_release_epoch != release_epoch_.load(std::memory_order_acquire);
        const bool safety_now = interface_state.safety_required.load(std::memory_order_acquire) ||
                                release_requested_.load(std::memory_order_acquire);
        const bool any_safety_pending = any_safety_required();
        if (slot_generation != current_generation ||
            slot_authority_epoch != current_authority_epoch ||
            !mounted_and_active(interface) ||
            stale_unsafe || ((safety_now || any_safety_pending) && !safety_kind)) {
            if (!safety_kind &&
                (safety_now || unsafe_report_holds_state(kind,
                                                          interface_state.slot_report,
                                                          length))) {
                interface_state.safety_required.store(true, std::memory_order_release);
            }
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            continue;
        }
        const bool accepted = submit(context, static_cast<std::uint8_t>(interface),
                                     interface_state.slot_report, length);
        if (!accepted) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            // Unsafe reports are discarded. Safety reports remain required and
            // are retried only in the safe all-up direction.
            continue;
        }
        interface_state.in_flight_generation = current_generation;
        interface_state.in_flight_authority_epoch = current_authority_epoch;
        interface_state.in_flight_kind = kind;
        interface_state.in_flight_length = length;
        std::memcpy(interface_state.in_flight_report, interface_state.slot_report, length);
        interface_state.in_flight.store(true, std::memory_order_release);
        // Submission is provisional: record the intended logical state now so
        // a detach, lease expiry, or takeover racing completion still derives
        // the need for an all-up safety report from the attempted operation.
        if (kind == ReportKind::kUnsafeKeyboard) {
            interface_state.keyboard.modifiers = interface_state.slot_report[0];
            for (std::size_t report_index = 0;
                 report_index < interface_state.keyboard.keycodes.size(); ++report_index) {
                interface_state.keyboard.keycodes[report_index] = interface_state.slot_report[report_index + 2];
            }
        } else if (kind == ReportKind::kUnsafeMouse) {
            interface_state.mouse.buttons = static_cast<std::uint8_t>(interface_state.slot_report[0] & 0x1fU);
        } else {
            interface_state.keyboard = {};
            interface_state.mouse = {};
        }
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        break;
    }
}

bool StateMachine::report_complete(std::uint8_t instance) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    InterfaceState &interface_state = interfaces_[instance];
    if (!interface_state.in_flight.load(std::memory_order_acquire) ||
        interface_state.in_flight_generation != attach_generation() ||
        interface_state.in_flight_authority_epoch != authority_epoch()) {
        return false;
    }
    const ReportKind kind = interface_state.in_flight_kind;
    interface_state.in_flight.store(false, std::memory_order_release);
    if (kind == ReportKind::kSafetyKeyboard || kind == ReportKind::kSafetyMouse) {
        interface_state.safety_required.store(false, std::memory_order_release);
        interface_state.host_state_uncertain.store(false, std::memory_order_release);
        interface_state.keyboard = {};
        interface_state.mouse = {};
    } else {
        interface_state.host_state_uncertain.store(false, std::memory_order_release);
    }
    return true;
}

bool StateMachine::report_failed(std::uint8_t instance) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    InterfaceState &interface_state = interfaces_[instance];
    if (!interface_state.in_flight.load(std::memory_order_acquire) ||
        interface_state.in_flight_generation != attach_generation() ||
        interface_state.in_flight_authority_epoch != authority_epoch()) {
        return false;
    }
    interface_state.in_flight.store(false, std::memory_order_release);
    interface_state.host_state_uncertain.store(true, std::memory_order_release);
    interface_state.safety_required.store(true, std::memory_order_release);
    return true;
}

KeyboardState StateMachine::keyboard_state() const {
    return state(Interface::kKeyboard).keyboard;
}

MouseState StateMachine::mouse_state() const {
    return state(Interface::kMouse).mouse;
}

bool StateMachine::safety_required(Interface interface) const {
    return state(interface).safety_required.load(std::memory_order_acquire);
}

bool StateMachine::host_state_uncertain(Interface interface) const {
    return state(interface).host_state_uncertain.load(std::memory_order_acquire);
}

bool StateMachine::report_in_flight(Interface interface) const {
    return state(interface).in_flight.load(std::memory_order_acquire);
}

#ifndef HID_RUNTIME_NATIVE_TEST
void Runtime::initialize() {
    state_machine_.on_unmount();
    result_bits_.store(0, std::memory_order_release);
}

void Runtime::on_mount() {
    state_machine_.on_mount();
    // The ESP32-S3 DWC2 controller clears its SOF enable state during the
    // enumeration bus reset. Re-arm only after TinyUSB reports this mount so
    // every configured attach gets the task-affine readiness refresh.
    tud_sof_cb_enable(true);
    result_bits_.store(0, std::memory_order_release);
}
void Runtime::on_unmount() {
    state_machine_.on_unmount();
    result_bits_.store(0, std::memory_order_release);
}
void Runtime::on_suspend() { state_machine_.on_suspend(); }
void Runtime::on_resume() { state_machine_.on_resume(); }

StatusSnapshot Runtime::status_snapshot() const { return state_machine_.status(); }

AuthorityEpoch Runtime::authority_epoch() const { return state_machine_.authority_epoch(); }

bool Runtime::queue_keyboard_report(std::uint8_t modifiers,
                                    const std::array<std::uint8_t, 6> &keycodes) {
    return state_machine_.queue_keyboard_report(modifiers, keycodes);
}

bool Runtime::queue_mouse_report(std::uint8_t buttons, std::int8_t x, std::int8_t y,
                                 std::int8_t vertical, std::int8_t horizontal) {
    return state_machine_.queue_mouse_report(buttons, x, y, vertical, horizontal);
}

void Runtime::request_release_all() { state_machine_.request_release_all(); }

bool Runtime::submit_report(void *, std::uint8_t instance, const std::uint8_t *report,
                            std::uint16_t length) {
    // The state machine has already checked lifecycle/readiness. This adapter
    // is called only from the public tud_sof_cb path in TinyUSB task context.
    return tud_hid_n_report(instance, 0, report, length);
}

void Runtime::service_sof() {
    state_machine_.set_ready(Interface::kKeyboard, tud_hid_n_ready(0));
    state_machine_.set_ready(Interface::kMouse, tud_hid_n_ready(1));
    state_machine_.execute(submit_report, nullptr);
}

void Runtime::set_result(Interface interface, bool failed) {
    const std::uint8_t index_bit = interface == Interface::kKeyboard ? 0 : 1;
    const std::uint8_t bit = static_cast<std::uint8_t>(1U << (index_bit + (failed ? 2 : 0)));
    result_bits_.fetch_or(bit, std::memory_order_release);
}

void Runtime::on_report_complete(std::uint8_t instance) {
    if (state_machine_.report_complete(instance) &&
        instance <= static_cast<std::uint8_t>(Interface::kMouse)) {
        set_result(static_cast<Interface>(instance), false);
    }
}

bool Runtime::on_report_failed(std::uint8_t instance) {
    if (state_machine_.report_failed(instance) &&
        instance <= static_cast<std::uint8_t>(Interface::kMouse)) {
        set_result(static_cast<Interface>(instance), true);
        return true;
    }
    return false;
}

bool Runtime::take_report_sent(Interface interface) {
    const std::uint8_t bit = interface == Interface::kKeyboard ? 1U : 2U;
    std::uint8_t current = result_bits_.load(std::memory_order_acquire);
    while ((current & bit) != 0 &&
           !result_bits_.compare_exchange_weak(current,
                                                static_cast<std::uint8_t>(current & ~bit),
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
    }
    return (current & bit) != 0;
}

bool Runtime::take_report_failed(Interface interface) {
    const std::uint8_t bit = interface == Interface::kKeyboard ? 4U : 8U;
    std::uint8_t current = result_bits_.load(std::memory_order_acquire);
    while ((current & bit) != 0 &&
           !result_bits_.compare_exchange_weak(current,
                                                static_cast<std::uint8_t>(current & ~bit),
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
    }
    return (current & bit) != 0;
}
#endif

}  // namespace hid_runtime
