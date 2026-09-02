#include "hid_runtime/hid_runtime.hpp"

#include <cstring>

#ifndef HID_RUNTIME_NATIVE_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

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
    interface_state.logical_state_held.store(false, std::memory_order_release);
    if (&interface_state == &interfaces_[0]) {
        const std::uint8_t all_up[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        write_confirmed_keyboard(all_up);
    } else {
        write_confirmed_mouse(0);
    }
    interface_state.keyboard = {};
    interface_state.mouse = {};
}

void StateMachine::write_confirmed_keyboard(const std::uint8_t *report) {
    InterfaceState &keyboard = interfaces_[0];
    keyboard.confirmed_sequence.fetch_add(1, std::memory_order_acq_rel);
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (report != nullptr) {
        for (std::size_t index = 0; index < 4; ++index) {
            low |= static_cast<std::uint32_t>(report[index]) << (index * 8U);
            high |= static_cast<std::uint32_t>(report[index + 4]) << (index * 8U);
        }
    }
    keyboard.confirmed_low.store(low, std::memory_order_relaxed);
    keyboard.confirmed_high.store(high, std::memory_order_relaxed);
    keyboard.confirmed_sequence.fetch_add(1, std::memory_order_release);
}

std::array<std::uint8_t, 8> StateMachine::read_confirmed_keyboard() const {
    const InterfaceState &keyboard = interfaces_[0];
    std::array<std::uint8_t, 8> report{};
    while (true) {
        const std::uint32_t first = keyboard.confirmed_sequence.load(std::memory_order_acquire);
        if ((first & 1U) != 0) {
            continue;
        }
        const std::uint32_t low = keyboard.confirmed_low.load(std::memory_order_relaxed);
        const std::uint32_t high = keyboard.confirmed_high.load(std::memory_order_relaxed);
        const std::uint32_t second = keyboard.confirmed_sequence.load(std::memory_order_acquire);
        if (first == second && (second & 1U) == 0) {
            for (std::size_t index = 0; index < 4; ++index) {
                report[index] = static_cast<std::uint8_t>(low >> (index * 8U));
                report[index + 4] = static_cast<std::uint8_t>(high >> (index * 8U));
            }
            return report;
        }
    }
}

bool StateMachine::confirmed_keyboard_equals(const std::uint8_t *report) const {
    if (report == nullptr) {
        return false;
    }
    return read_confirmed_keyboard() ==
           std::array<std::uint8_t, 8>{report[0], report[1], report[2], report[3],
                                      report[4], report[5], report[6], report[7]};
}

void StateMachine::write_confirmed_mouse(std::uint8_t buttons) {
    interfaces_[1].confirmed_mouse_buttons.store(
        static_cast<std::uint8_t>(buttons & 0x1fU), std::memory_order_release);
}

std::uint8_t StateMachine::read_confirmed_mouse() const {
    return interfaces_[1].confirmed_mouse_buttons.load(std::memory_order_acquire);
}

void StateMachine::cancel_release_ticket() {
    if (!release_ticket_.active.load(std::memory_order_acquire)) {
        return;
    }
    release_ticket_.canceled.store(true, std::memory_order_release);
    release_ticket_.keyboard.store(ReleaseAllInterfaceState::kCanceled,
                                   std::memory_order_release);
    release_ticket_.mouse.store(ReleaseAllInterfaceState::kCanceled,
                                std::memory_order_release);
    release_ticket_.active.store(false, std::memory_order_release);
}

void StateMachine::cancel_keyboard_ticket(KeyboardReportTicketOutcome outcome) {
    auto ticket_state = keyboard_ticket_.state.load(std::memory_order_acquire);
    while (ticket_state == KeyboardReportTicketState::kWriting ||
           ticket_state == KeyboardReportTicketState::kPublished) {
        if (keyboard_ticket_.state.compare_exchange_weak(
                ticket_state, KeyboardReportTicketState::kCanceled,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            keyboard_ticket_.outcome.store(outcome, std::memory_order_release);
            return;
        }
    }
}

void StateMachine::cancel_mouse_ticket(MouseReportTicketOutcome outcome) {
    auto ticket_state = mouse_ticket_.state.load(std::memory_order_acquire);
    while (ticket_state == MouseReportTicketState::kWriting ||
           ticket_state == MouseReportTicketState::kPublished) {
        if (mouse_ticket_.state.compare_exchange_weak(
                ticket_state, MouseReportTicketState::kCanceled,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            mouse_ticket_.outcome.store(outcome, std::memory_order_release);
            return;
        }
    }
}

bool StateMachine::known_all_up(Interface interface) const {
    const InterfaceState &interface_state = state(interface);
    if (interface_state.logical_state_held.load(std::memory_order_acquire) ||
        interface_state.in_flight.load(std::memory_order_acquire) ||
        interface_state.safety_required.load(std::memory_order_acquire) ||
        interface_state.host_state_uncertain.load(std::memory_order_acquire)) {
        return false;
    }
    const std::uint8_t slot_state = interface_state.slot_state.load(std::memory_order_acquire);
    if (slot_state == kSlotWriting || slot_state == kSlotReady ||
        slot_state == kSlotExecuting) {
        return false;
    }
    return true;
}

void StateMachine::set_release_outcome(Interface interface,
                                        ReleaseAllInterfaceState outcome) {
    if (interface == Interface::kKeyboard) {
        release_ticket_.keyboard.store(outcome, std::memory_order_release);
    } else {
        release_ticket_.mouse.store(outcome, std::memory_order_release);
    }
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
    if (!usb_lifecycle_.observe_mount()) {
        return;
    }
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    if (usb_lifecycle_.has_unresolved_prior_generation()) {
        // A reinstalled stack cannot prove the prior host observed all-up.
        // Keep a fresh-generation safety barrier until a new all-up completes.
        for (InterfaceState &interface_state : interfaces_) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            interface_state.in_flight.store(false, std::memory_order_release);
            interface_state.safety_required.store(true, std::memory_order_release);
            interface_state.host_state_uncertain.store(true, std::memory_order_release);
            interface_state.logical_state_held.store(false, std::memory_order_release);
        }
        request_release_all();
    } else {
        clear_interface(interfaces_[0]);
        clear_interface(interfaces_[1]);
    }
    release_request_generation_.store(0, std::memory_order_release);
    release_request_authority_epoch_.store(0, std::memory_order_release);
    release_request_epoch_.store(0, std::memory_order_release);
    release_requested_.store(false, std::memory_order_release);
    status_bits_.store(kMountedBit, std::memory_order_release);
    if (!usb_lifecycle_.snapshot().host_release_uncertain) {
        usb_lifecycle_.mark_release_confirmed();
    }
}

void StateMachine::on_unmount() {
    const UsbGeneration retired_generation = attach_generation();
    if (!usb_lifecycle_.observe_unmount()) {
        // Explicit uninstall calls tud_umount_cb() during caller-side
        // teardown. It is observational only: never change generation,
        // desired/observed state, or uncertainty here.
        if (usb_lifecycle_.snapshot().observed == usb_lifecycle::ObservedState::kDetaching) {
            status_bits_.store(0, std::memory_order_release);
        }
        return;
    }
    // Callback-side invalidation never waits for the shared control executor.
    // Its pending gate closes unsafe route work before later cleanup runs.
    (void)route_.invalidate();
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    // Invalidate first: work from an older attach or authority epoch can never
    // be accepted by a later executor pass, even if it races this callback.
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    status_bits_.store(0, std::memory_order_release);
    bool uncertainty = false;
    for (InterfaceState &interface_state : interfaces_) {
        const bool needs_safety =
            interface_state.logical_state_held.load(std::memory_order_acquire) ||
            interface_state.in_flight.load(std::memory_order_acquire) ||
            interface_state.host_state_uncertain.load(std::memory_order_acquire);
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        interface_state.in_flight.store(false, std::memory_order_release);
        if (needs_safety) {
            uncertainty = true;
            interface_state.safety_required.store(true, std::memory_order_release);
            interface_state.host_state_uncertain.store(true, std::memory_order_release);
            interface_state.logical_state_held.store(false, std::memory_order_release);
        } else {
            clear_interface(interface_state);
        }
    }
    if (uncertainty) {
        usb_lifecycle_.mark_release_uncertain_for_generation(retired_generation);
    }
    release_request_generation_.store(0, std::memory_order_release);
    release_request_authority_epoch_.store(0, std::memory_order_release);
    release_request_epoch_.store(0, std::memory_order_release);
    release_requested_.store(false, std::memory_order_release);
}

void StateMachine::on_suspend() {
    if (!usb_lifecycle_.observe_suspend()) {
        return;
    }
    (void)route_.invalidate();
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
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
    if (!usb_lifecycle_.observe_resume()) {
        return;
    }
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
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
    apply_u7_1b_compatibility_route();
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

UsbTransitionOutcome StateMachine::request_usb_attach(usb_lifecycle::Executor &executor) {
    const StatusSnapshot pre_transition_runtime = status();
    const usb_lifecycle::TransitionOutcome transition = usb_lifecycle_.request_attach(executor);
    if (transition.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        return UsbTransitionOutcome{
            .action_result = transition.action_result,
            .snapshot_valid = transition.snapshot_valid,
            .lifecycle = transition.snapshot,
            .runtime = pre_transition_runtime,
        };
    }
    // Request intent is the fail-closed authority boundary. U7.1A does not
    // invoke an executor in firmware, so this has no current USB behavior.
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    status_bits_.store(0, std::memory_order_release);
    return UsbTransitionOutcome{
        .action_result = transition.action_result,
        .snapshot_valid = true,
        .lifecycle = transition.snapshot,
        // An accepted attach is always reported before install/mount progress.
        .runtime = {},
    };
}

UsbTransitionOutcome StateMachine::request_usb_detach(usb_lifecycle::Executor &executor) {
    const UsbGeneration retired_generation = attach_generation();
    const StatusSnapshot stage_a_runtime = status();
    const usb_lifecycle::TransitionOutcome transition = usb_lifecycle_.request_detach(executor);
    if (transition.action_result != usb_lifecycle::TransitionResult::kAccepted) {
        return UsbTransitionOutcome{
            .action_result = transition.action_result,
            .snapshot_valid = transition.snapshot_valid,
            .lifecycle = transition.snapshot,
            .runtime = stage_a_runtime,
        };
    }
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
    // Stage A retains mounted/readiness bits so only lifecycle-owned all-up
    // work can execute in the still-current installed generation. Unsafe work
    // is already rejected by desired=hidden and the fresh authority epoch.
    for (InterfaceState &interface_state : interfaces_) {
        if (interface_state.logical_state_held.load(std::memory_order_acquire) ||
            interface_state.in_flight.load(std::memory_order_acquire) ||
            interface_state.host_state_uncertain.load(std::memory_order_acquire)) {
            interface_state.safety_required.store(true, std::memory_order_release);
            interface_state.host_state_uncertain.store(true, std::memory_order_release);
        }
        interface_state.in_flight.store(false, std::memory_order_release);
        std::uint8_t expected = kSlotReady;
        interface_state.slot_state.compare_exchange_strong(
            expected, kSlotCanceled, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    (void)retired_generation;
    return UsbTransitionOutcome{
        .action_result = transition.action_result,
        .snapshot_valid = true,
        .lifecycle = transition.snapshot,
        .runtime = stage_a_runtime,
    };
}

usb_lifecycle::Snapshot StateMachine::usb_lifecycle_snapshot() const {
    return usb_lifecycle_.snapshot();
}

hid_route::Snapshot StateMachine::route_snapshot() const { return route_.snapshot(); }

bool StateMachine::route_usb_ready(const hid_route::Snapshot &route,
                                   const usb_lifecycle::Snapshot &lifecycle,
                                   const StatusSnapshot &runtime) const {
    return route.coherent && !route.invalidation_pending &&
           route.desired == hid_route::OutputRoute::kUsb &&
           route.active == hid_route::OutputRoute::kUsb &&
           route.transition == hid_route::Transition::kStable &&
           lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed &&
           lifecycle.observed == usb_lifecycle::ObservedState::kMounted &&
           runtime.mounted && !runtime.suspended && runtime.keyboard_ready &&
           runtime.mouse_ready && !any_safety_required() &&
           !lifecycle.safety_pending && !lifecycle.host_release_uncertain &&
           !lifecycle.recovery_required;
}

RouteStatusSnapshot StateMachine::route_status_snapshot() const {
    constexpr unsigned kMaxAttempts = 3;
    hid_route::Snapshot fallback = route_.snapshot();
    for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const hid_route::Snapshot route_before = route_.snapshot();
        const usb_lifecycle::Snapshot lifecycle_before = usb_lifecycle_.snapshot();
        const StatusSnapshot runtime_before = status();
        const usb_lifecycle::Snapshot lifecycle_after = usb_lifecycle_.snapshot();
        const StatusSnapshot runtime_after = status();
        const hid_route::Snapshot route_after = route_.snapshot();
        fallback = route_after;
        const bool route_stable = route_before.coherent && route_after.coherent &&
                                  route_before.desired == route_after.desired &&
                                  route_before.active == route_after.active &&
                                  route_before.generation == route_after.generation &&
                                  route_before.transition == route_after.transition &&
                                  route_before.invalidation_pending ==
                                      route_after.invalidation_pending;
        const bool lifecycle_stable =
            lifecycle_before.desired == lifecycle_after.desired &&
            lifecycle_before.observed == lifecycle_after.observed &&
            lifecycle_before.generation == lifecycle_after.generation &&
            lifecycle_before.safety_pending == lifecycle_after.safety_pending &&
            lifecycle_before.host_release_uncertain ==
                lifecycle_after.host_release_uncertain &&
            lifecycle_before.recovery_required == lifecycle_after.recovery_required;
        const bool runtime_stable =
            runtime_before.mounted == runtime_after.mounted &&
            runtime_before.suspended == runtime_after.suspended &&
            runtime_before.keyboard_ready == runtime_after.keyboard_ready &&
            runtime_before.mouse_ready == runtime_after.mouse_ready;
        if (route_stable && lifecycle_stable && runtime_stable) {
            return RouteStatusSnapshot{
                .route = route_after,
                .ready = route_usb_ready(route_after, lifecycle_after, runtime_after),
            };
        }
    }
    return RouteStatusSnapshot{.route = fallback, .ready = false};
}

void StateMachine::retire_unsafe_route_authority() {
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    authority_epoch_.fetch_add(1, std::memory_order_acq_rel);
    release_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

RouteTransitionOutcome StateMachine::request_route_usb() {
    const RouteStatusSnapshot before = route_status_snapshot();
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    if (before.route.desired == hid_route::OutputRoute::kUsb &&
        before.route.active == hid_route::OutputRoute::kUsb &&
        before.route.transition == hid_route::Transition::kStable) {
        const RouteTransitionResult result =
            before.ready ? RouteTransitionResult::kNoOp
            : lifecycle.safety_pending || lifecycle.host_release_uncertain
                ? RouteTransitionResult::kSafetyPending
                : RouteTransitionResult::kNotReady;
        return RouteTransitionOutcome{.action_result = result,
                                      .snapshot_valid = true,
                                      .snapshot = before};
    }
    if (!before.route.coherent || before.route.invalidation_pending ||
        before.route.desired != hid_route::OutputRoute::kNone ||
        before.route.active != hid_route::OutputRoute::kNone ||
        before.route.transition != hid_route::Transition::kStable) {
        return {};
    }
    const StatusSnapshot runtime = status();
    if (lifecycle.safety_pending || lifecycle.host_release_uncertain ||
        any_safety_required()) {
        return RouteTransitionOutcome{.action_result = RouteTransitionResult::kSafetyPending,
                                      .snapshot_valid = true,
                                      .snapshot = before};
    }
    if (lifecycle.desired != usb_lifecycle::DesiredExposure::kExposed ||
        lifecycle.observed != usb_lifecycle::ObservedState::kMounted ||
        lifecycle.recovery_required || !runtime.mounted || runtime.suspended ||
        !runtime.keyboard_ready || !runtime.mouse_ready) {
        return RouteTransitionOutcome{.action_result = RouteTransitionResult::kNotReady,
                                      .snapshot_valid = true,
                                      .snapshot = before};
    }
    retire_unsafe_route_authority();
    if (!route_.commit_usb_if_none()) {
        return RouteTransitionOutcome{.action_result = RouteTransitionResult::kNotReady,
                                      .snapshot_valid = true,
                                      .snapshot = route_status_snapshot()};
    }
    return RouteTransitionOutcome{.action_result = RouteTransitionResult::kAccepted,
                                  .snapshot_valid = true,
                                  .snapshot = route_status_snapshot()};
}

RouteTransitionOutcome StateMachine::request_route_none() {
    const RouteStatusSnapshot before = route_status_snapshot();
    if (before.route.desired == hid_route::OutputRoute::kNone &&
        before.route.active == hid_route::OutputRoute::kNone &&
        before.route.transition == hid_route::Transition::kStable) {
        return RouteTransitionOutcome{.action_result = RouteTransitionResult::kNoOp,
                                      .snapshot_valid = true,
                                      .snapshot = before};
    }
    if (!before.route.coherent || before.route.invalidation_pending ||
        before.route.desired != hid_route::OutputRoute::kUsb ||
        before.route.active != hid_route::OutputRoute::kUsb ||
        before.route.transition != hid_route::Transition::kStable) {
        return {};
    }

    hid_route::Snapshot stage_a{};
    if (!route_.begin_usb_release(&stage_a)) {
        return {};
    }
    retire_unsafe_route_authority();
    bool requires_safety = false;
    for (const Interface interface : {Interface::kKeyboard, Interface::kMouse}) {
        InterfaceState &interface_state = state(interface);
        const bool needs_safety =
            !known_all_up(interface) ||
            interface_state.in_flight.load(std::memory_order_acquire) ||
            interface_state.host_state_uncertain.load(std::memory_order_acquire);
        if (needs_safety) {
            requires_safety = true;
            interface_state.safety_required.store(true, std::memory_order_release);
            interface_state.host_state_uncertain.store(true, std::memory_order_release);
        }
        interface_state.in_flight.store(false, std::memory_order_release);
        std::uint8_t expected = kSlotReady;
        interface_state.slot_state.compare_exchange_strong(
            expected, kSlotCanceled, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    const RouteStatusSnapshot accepted{.route = stage_a, .ready = false};
    if (!requires_safety) {
        usb_lifecycle_.mark_release_confirmed();
        (void)route_.complete_usb_release_if_matches(stage_a);
    }
    return RouteTransitionOutcome{.action_result = RouteTransitionResult::kAccepted,
                                  .snapshot_valid = true,
                                  .async_required = requires_safety,
                                  .snapshot = accepted};
}

void StateMachine::terminalize_route_release_schedule_failure(
    hid_route::Snapshot stage_a) {
    mark_lifecycle_detach_uncertain(attach_generation());
    (void)route_.complete_usb_release_if_matches(stage_a);
}

void StateMachine::complete_route_release(hid_route::Snapshot stage_a) {
    (void)route_.complete_usb_release_if_matches(stage_a);
}

UsbGeneration StateMachine::attach_generation() const {
    return usb_lifecycle_.generation();
}

AuthorityEpoch StateMachine::authority_epoch() const {
    return authority_epoch_.load(std::memory_order_acquire);
}

#ifdef HID_RUNTIME_NATIVE_TEST
void StateMachine::set_before_ticket_publish_hook_for_test(TestHook hook) {
    before_ticket_publish_hook_ = hook;
}

void StateMachine::set_before_submit_hook_for_test(TestHook hook) {
    before_submit_hook_ = hook;
}

void StateMachine::set_before_release_reconciliation_hook_for_test(TestHook hook) {
    before_release_reconciliation_hook_ = hook;
}

void StateMachine::publish_release_request_only_for_test() {
    publish_release_request();
}

bool StateMachine::release_requested_for_test() const {
    return release_requested_.load(std::memory_order_acquire);
}

std::uint32_t StateMachine::release_request_epoch_for_test() const {
    return release_request_epoch_.load(std::memory_order_acquire);
}
#endif

bool StateMachine::mounted_and_active(Interface interface) const {
    const StatusSnapshot snapshot = status();
    return !usb_lifecycle_.has_unresolved_prior_generation() &&
           usb_lifecycle_.accepts_hid(
        snapshot.mounted && !snapshot.suspended &&
        (interface == Interface::kKeyboard ? snapshot.keyboard_ready : snapshot.mouse_ready));
}

bool StateMachine::unsafe_route_active(RouteGeneration generation,
                                       HidTransport transport) const {
    return transport == HidTransport::kUsb &&
           route_.matches(hid_route::OutputRoute::kUsb, generation);
}

bool StateMachine::compatibility_usb_route_can_select() const {
    const StatusSnapshot snapshot = status();
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    return snapshot.mounted && !snapshot.suspended &&
           (snapshot.keyboard_ready || snapshot.mouse_ready) &&
           !any_safety_required() && !lifecycle.safety_pending &&
           !lifecycle.host_release_uncertain && !lifecycle.recovery_required &&
           lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed &&
           lifecycle.observed == usb_lifecycle::ObservedState::kMounted;
}

void StateMachine::apply_u7_1b_compatibility_route() {
    // U7.2A-only policy: this is the one removable compatibility seam. Public
    // Model B selection remains deferred, so attach never selects before a
    // clean mounted/readiness state exists.
    if (compatibility_usb_route_can_select()) {
        (void)route_.commit_usb_if_none();
    }
}

bool StateMachine::safety_transport_active(Interface interface) const {
    const StatusSnapshot snapshot = status();
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    const bool endpoint_ready = interface == Interface::kKeyboard
                                    ? snapshot.keyboard_ready
                                    : snapshot.mouse_ready;
    return snapshot.mounted && !snapshot.suspended && endpoint_ready &&
           !lifecycle.recovery_required &&
           ((lifecycle.desired == usb_lifecycle::DesiredExposure::kExposed &&
             lifecycle.observed == usb_lifecycle::ObservedState::kMounted) ||
            (lifecycle.desired == usb_lifecycle::DesiredExposure::kHidden &&
             lifecycle.observed == usb_lifecycle::ObservedState::kDetaching));
}

bool StateMachine::any_safety_required() const {
    return interfaces_[0].safety_required.load(std::memory_order_acquire) ||
           interfaces_[1].safety_required.load(std::memory_order_acquire);
}

bool StateMachine::active_release_request_is_current(UsbGeneration generation,
                                                     AuthorityEpoch authority_epoch,
                                                     std::uint32_t release_epoch) const {
    return release_requested_.load(std::memory_order_acquire) &&
           release_request_generation_.load(std::memory_order_acquire) == generation &&
           release_request_authority_epoch_.load(std::memory_order_acquire) == authority_epoch &&
           release_request_epoch_.load(std::memory_order_acquire) == release_epoch &&
           attach_generation() == generation &&
           this->authority_epoch() == authority_epoch &&
           release_epoch_.load(std::memory_order_acquire) == release_epoch;
}

bool StateMachine::release_request_is_current(UsbGeneration generation,
                                              AuthorityEpoch authority_epoch,
                                              std::uint32_t release_epoch) const {
    return !release_requested_.load(std::memory_order_acquire) &&
           release_request_generation_.load(std::memory_order_acquire) == generation &&
           release_request_authority_epoch_.load(std::memory_order_acquire) == authority_epoch &&
           release_request_epoch_.load(std::memory_order_acquire) == release_epoch &&
           attach_generation() == generation &&
           this->authority_epoch() == authority_epoch &&
           release_epoch_.load(std::memory_order_acquire) == release_epoch;
}

bool StateMachine::unavailable_usb_transport_is_clean() const {
    const StatusSnapshot runtime = status();
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    const bool unavailable =
        lifecycle.observed == usb_lifecycle::ObservedState::kDriverNotInstalled ||
        lifecycle.observed == usb_lifecycle::ObservedState::kDisconnected;
    return unavailable && !runtime.mounted && !runtime.suspended &&
           !lifecycle.recovery_required && !lifecycle.host_release_uncertain &&
           known_all_up(Interface::kKeyboard) && known_all_up(Interface::kMouse);
}

void StateMachine::reconcile_zero_work_release(UsbGeneration generation,
                                               AuthorityEpoch authority_epoch,
                                               std::uint32_t release_epoch,
                                               bool require_unavailable_transport) {
    if (!release_request_is_current(generation, authority_epoch, release_epoch) ||
        !known_all_up(Interface::kKeyboard) || !known_all_up(Interface::kMouse) ||
        usb_lifecycle_.snapshot().host_release_uncertain ||
        usb_lifecycle_.snapshot().recovery_required ||
        (require_unavailable_transport && !unavailable_usb_transport_is_clean())) {
        return;
    }
#ifdef HID_RUNTIME_NATIVE_TEST
    if (before_release_reconciliation_hook_ != nullptr) {
        before_release_reconciliation_hook_(this);
    }
#endif
    if (!release_request_is_current(generation, authority_epoch, release_epoch) ||
        !known_all_up(Interface::kKeyboard) || !known_all_up(Interface::kMouse) ||
        usb_lifecycle_.snapshot().host_release_uncertain ||
        usb_lifecycle_.snapshot().recovery_required ||
        (require_unavailable_transport && !unavailable_usb_transport_is_clean())) {
        return;
    }
    (void)usb_lifecycle_.clear_release_pending_if_not_uncertain();
    // A newer producer may have published after the pre-clear identity check.
    // Reassert its barrier rather than letting an older zero-work pass erase it.
    const usb_lifecycle::Snapshot after_clear = usb_lifecycle_.snapshot();
    if (release_requested_.load(std::memory_order_acquire) ||
        !known_all_up(Interface::kKeyboard) || !known_all_up(Interface::kMouse) ||
        after_clear.host_release_uncertain || after_clear.recovery_required) {
        usb_lifecycle_.mark_release_pending();
    }
}

void StateMachine::reconcile_unavailable_zero_work_release() {
    bool expected = false;
    if (!unavailable_release_reconciler_active_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    do {
        while (release_requested_.load(std::memory_order_acquire) &&
               unavailable_usb_transport_is_clean()) {
            const UsbGeneration generation =
                release_request_generation_.load(std::memory_order_acquire);
            const AuthorityEpoch request_authority_epoch =
                release_request_authority_epoch_.load(std::memory_order_acquire);
            const std::uint32_t request_release_epoch =
                release_request_epoch_.load(std::memory_order_acquire);
            if (!active_release_request_is_current(
                    generation, request_authority_epoch, request_release_epoch)) {
                break;
            }

            release_requested_.store(false, std::memory_order_release);
            if (!release_request_is_current(
                    generation, request_authority_epoch, request_release_epoch)) {
                const UsbGeneration current_generation = attach_generation();
                const AuthorityEpoch current_authority_epoch = authority_epoch();
                const std::uint32_t current_release_epoch =
                    release_epoch_.load(std::memory_order_acquire);
                if (release_request_generation_.load(std::memory_order_acquire) ==
                        current_generation &&
                    release_request_authority_epoch_.load(std::memory_order_acquire) ==
                        current_authority_epoch &&
                    release_request_epoch_.load(std::memory_order_acquire) ==
                        current_release_epoch) {
                    release_requested_.store(true, std::memory_order_release);
                    usb_lifecycle_.mark_release_pending();
                }
                continue;
            }
            reconcile_zero_work_release(
                generation, request_authority_epoch, request_release_epoch, true);
        }

        unavailable_release_reconciler_active_.store(false, std::memory_order_release);
        if (!release_requested_.load(std::memory_order_acquire) ||
            !unavailable_usb_transport_is_clean()) {
            return;
        }
        expected = false;
    } while (unavailable_release_reconciler_active_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_acquire));
}

bool StateMachine::queue_report(Interface interface, ReportKind kind,
                                const std::uint8_t *report, std::uint8_t length) {
    if (report == nullptr || length == 0 || length > 8 ||
        !mounted_and_active(interface)) {
        return false;
    }
    const UsbGeneration queue_generation = attach_generation();
    const AuthorityEpoch queue_authority_epoch = authority_epoch();
    const hid_route::Snapshot queue_route = route_.snapshot();
    InterfaceState &interface_state = state(interface);
    const std::uint32_t release_epoch = release_epoch_.load(std::memory_order_acquire);
    if (kind != ReportKind::kSafetyKeyboard && kind != ReportKind::kSafetyMouse &&
        (release_requested_.load(std::memory_order_acquire) || any_safety_required() ||
         !unsafe_route_active(queue_route.generation, HidTransport::kUsb))) {
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
    interface_state.slot_transport_generation = queue_generation;
    interface_state.slot_authority_epoch = queue_authority_epoch;
    interface_state.slot_route_generation = queue_route.generation;
    interface_state.slot_transport = HidTransport::kUsb;
    interface_state.slot_ticket_id = next_ticket_id_.fetch_add(1, std::memory_order_acq_rel);
    interface_state.slot_release_epoch = release_epoch;
    interface_state.slot_kind = kind;
    interface_state.slot_length = length;
    std::memcpy(interface_state.slot_report, report, length);
    if (release_requested_.load(std::memory_order_acquire) ||
        release_epoch_.load(std::memory_order_acquire) != release_epoch ||
        attach_generation() != queue_generation ||
        authority_epoch() != queue_authority_epoch ||
        (kind != ReportKind::kSafetyKeyboard && kind != ReportKind::kSafetyMouse &&
         !unsafe_route_active(queue_route.generation, HidTransport::kUsb)) ||
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

KeyboardReportBeginResult StateMachine::begin_keyboard_report(
    std::uint8_t modifiers, const std::array<std::uint8_t, 6> &keycodes) {
    // Reap only terminal ticket states. A submitted ticket may still have a
    // report-complete callback pending, but report_in_flight remains the
    // authoritative busy barrier for a replacement request.
    auto ticket_state = keyboard_ticket_.state.load(std::memory_order_acquire);
    while (ticket_state == KeyboardReportTicketState::kSubmitted ||
           ticket_state == KeyboardReportTicketState::kNotReady ||
           ticket_state == KeyboardReportTicketState::kCanceled) {
        if (keyboard_ticket_.state.compare_exchange_weak(
                ticket_state, KeyboardReportTicketState::kFree,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ticket_state = KeyboardReportTicketState::kFree;
            break;
        }
    }
    if (ticket_state != KeyboardReportTicketState::kFree) {
        return KeyboardReportBeginResult::kBusy;
    }

    const StatusSnapshot snapshot = status();
    const hid_route::Snapshot route = route_.snapshot();
    if (!snapshot.mounted || snapshot.suspended || !snapshot.keyboard_ready ||
        !unsafe_route_active(route.generation, HidTransport::kUsb)) {
        return KeyboardReportBeginResult::kNotReady;
    }
    if (release_requested_.load(std::memory_order_acquire) || any_safety_required()) {
        return KeyboardReportBeginResult::kSafetyPending;
    }
    InterfaceState &keyboard = state(Interface::kKeyboard);
    if (keyboard.in_flight.load(std::memory_order_acquire) ||
        keyboard.slot_state.load(std::memory_order_acquire) != kSlotEmpty) {
        return KeyboardReportBeginResult::kBusy;
    }

    const std::uint8_t report[8] = {
        modifiers, 0, keycodes[0], keycodes[1], keycodes[2], keycodes[3], keycodes[4], keycodes[5],
    };
    const bool desired_all_up = modifiers == 0 && keycodes == std::array<std::uint8_t, 6>{};
    if (!desired_all_up && confirmed_keyboard_equals(report) &&
        !keyboard.host_state_uncertain.load(std::memory_order_acquire) &&
        !keyboard.safety_required.load(std::memory_order_acquire)) {
        return KeyboardReportBeginResult::kAlreadySet;
    }

    KeyboardReportTicketState expected = KeyboardReportTicketState::kFree;
    if (!keyboard_ticket_.state.compare_exchange_strong(
            expected,
            KeyboardReportTicketState::kWriting,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return KeyboardReportBeginResult::kBusy;
    }
    const UsbGeneration generation = attach_generation();
    const AuthorityEpoch epoch = authority_epoch();
    const std::uint32_t release_epoch = release_epoch_.load(std::memory_order_acquire);
    keyboard_ticket_.transport_generation.store(generation, std::memory_order_relaxed);
    keyboard_ticket_.authority_epoch.store(epoch, std::memory_order_relaxed);
    keyboard_ticket_.route_generation.store(route.generation, std::memory_order_relaxed);
    keyboard_ticket_.transport.store(HidTransport::kUsb, std::memory_order_relaxed);
    keyboard_ticket_.ticket_id.store(next_ticket_id_.fetch_add(1, std::memory_order_acq_rel),
                                     std::memory_order_relaxed);
    keyboard_ticket_.release_epoch.store(release_epoch, std::memory_order_relaxed);
    std::memcpy(keyboard_ticket_.report, report, sizeof(report));
    keyboard_ticket_.outcome.store(KeyboardReportTicketOutcome::kNone, std::memory_order_relaxed);

#ifdef HID_RUNTIME_NATIVE_TEST
    if (before_ticket_publish_hook_ != nullptr) {
        before_ticket_publish_hook_(this);
    }
#endif

    if (generation != attach_generation() || epoch != authority_epoch() ||
        release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        !unsafe_route_active(route.generation, HidTransport::kUsb) ||
        !mounted_and_active(Interface::kKeyboard) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required() ||
        keyboard.in_flight.load(std::memory_order_acquire) ||
        keyboard.slot_state.load(std::memory_order_acquire) != kSlotEmpty) {
        keyboard_ticket_.outcome.store(
            generation != attach_generation() || epoch != authority_epoch()
                ? KeyboardReportTicketOutcome::kAuthorityLost
                : KeyboardReportTicketOutcome::kSafetyPending,
            std::memory_order_release);
        keyboard_ticket_.state.store(KeyboardReportTicketState::kCanceled,
                                     std::memory_order_release);
        return generation != attach_generation() || epoch != authority_epoch()
                   ? KeyboardReportBeginResult::kAuthorityLost
                   : KeyboardReportBeginResult::kSafetyPending;
    }
    KeyboardReportTicketState publishing = KeyboardReportTicketState::kWriting;
    if (!keyboard_ticket_.state.compare_exchange_strong(
            publishing, KeyboardReportTicketState::kPublished,
            std::memory_order_release, std::memory_order_acquire)) {
        // A lifecycle/safety callback won the WRITING -> CANCELED race. Do
        // not resurrect that ticket by storing PUBLISHED after cancellation.
        const KeyboardReportTicketOutcome canceled_outcome =
            keyboard_ticket_.outcome.load(std::memory_order_acquire);
        return canceled_outcome == KeyboardReportTicketOutcome::kAuthorityLost
                   ? KeyboardReportBeginResult::kAuthorityLost
                   : KeyboardReportBeginResult::kSafetyPending;
    }
    return KeyboardReportBeginResult::kPublished;
}

KeyboardReportSnapshot StateMachine::keyboard_report_snapshot() const {
    return KeyboardReportSnapshot{
        .state = keyboard_ticket_.state.load(std::memory_order_acquire),
        .outcome = keyboard_ticket_.outcome.load(std::memory_order_acquire),
    };
}

bool StateMachine::cancel_keyboard_report() {
    KeyboardReportTicketState expected = KeyboardReportTicketState::kPublished;
    if (!keyboard_ticket_.state.compare_exchange_strong(
            expected, KeyboardReportTicketState::kCanceled,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    keyboard_ticket_.outcome.store(KeyboardReportTicketOutcome::kNotReady,
                                   std::memory_order_release);
    return true;
}

void StateMachine::finalize_keyboard_report() {
    auto state = keyboard_ticket_.state.load(std::memory_order_acquire);
    while (state == KeyboardReportTicketState::kSubmitted ||
           state == KeyboardReportTicketState::kNotReady ||
           state == KeyboardReportTicketState::kCanceled) {
        if (keyboard_ticket_.state.compare_exchange_weak(
                state, KeyboardReportTicketState::kFree,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

MouseReportBeginResult StateMachine::begin_mouse_report(
    std::uint8_t buttons, std::int8_t x, std::int8_t y, std::int8_t vertical,
    std::int8_t horizontal) {
    // Reap only terminal ticket states. The in-flight interface bit remains
    // the authoritative barrier until TinyUSB reports completion.
    auto ticket_state = mouse_ticket_.state.load(std::memory_order_acquire);
    while (ticket_state == MouseReportTicketState::kSubmitted ||
           ticket_state == MouseReportTicketState::kNotReady ||
           ticket_state == MouseReportTicketState::kCanceled) {
        if (mouse_ticket_.state.compare_exchange_weak(
                ticket_state, MouseReportTicketState::kFree,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ticket_state = MouseReportTicketState::kFree;
            break;
        }
    }
    if (ticket_state != MouseReportTicketState::kFree) {
        return MouseReportBeginResult::kBusy;
    }

    const StatusSnapshot snapshot = status();
    const hid_route::Snapshot route = route_.snapshot();
    if (!snapshot.mounted || snapshot.suspended || !snapshot.mouse_ready ||
        !unsafe_route_active(route.generation, HidTransport::kUsb)) {
        return MouseReportBeginResult::kNotReady;
    }
    if (release_requested_.load(std::memory_order_acquire) || any_safety_required()) {
        return MouseReportBeginResult::kSafetyPending;
    }
    InterfaceState &mouse = state(Interface::kMouse);
    if (mouse.in_flight.load(std::memory_order_acquire) ||
        mouse.slot_state.load(std::memory_order_acquire) != kSlotEmpty) {
        return MouseReportBeginResult::kBusy;
    }

    const bool no_relative_delta = x == 0 && y == 0 && vertical == 0 && horizontal == 0;
    if (no_relative_delta &&
        static_cast<std::uint8_t>(buttons & 0x1fU) == read_confirmed_mouse() &&
        !mouse.host_state_uncertain.load(std::memory_order_acquire) &&
        !mouse.safety_required.load(std::memory_order_acquire)) {
        return MouseReportBeginResult::kAlreadySet;
    }

    MouseReportTicketState expected = MouseReportTicketState::kFree;
    if (!mouse_ticket_.state.compare_exchange_strong(
            expected, MouseReportTicketState::kWriting,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return MouseReportBeginResult::kBusy;
    }
    const UsbGeneration generation = attach_generation();
    const AuthorityEpoch epoch = authority_epoch();
    const std::uint32_t release_epoch = release_epoch_.load(std::memory_order_acquire);
    mouse_ticket_.transport_generation.store(generation, std::memory_order_relaxed);
    mouse_ticket_.authority_epoch.store(epoch, std::memory_order_relaxed);
    mouse_ticket_.route_generation.store(route.generation, std::memory_order_relaxed);
    mouse_ticket_.transport.store(HidTransport::kUsb, std::memory_order_relaxed);
    mouse_ticket_.ticket_id.store(next_ticket_id_.fetch_add(1, std::memory_order_acq_rel),
                                  std::memory_order_relaxed);
    mouse_ticket_.release_epoch.store(release_epoch, std::memory_order_relaxed);
    mouse_ticket_.report[0] = static_cast<std::uint8_t>(buttons & 0x1fU);
    mouse_ticket_.report[1] = static_cast<std::uint8_t>(x);
    mouse_ticket_.report[2] = static_cast<std::uint8_t>(y);
    mouse_ticket_.report[3] = static_cast<std::uint8_t>(vertical);
    mouse_ticket_.report[4] = static_cast<std::uint8_t>(horizontal);
    mouse_ticket_.outcome.store(MouseReportTicketOutcome::kNone, std::memory_order_relaxed);

#ifdef HID_RUNTIME_NATIVE_TEST
    if (before_ticket_publish_hook_ != nullptr) {
        before_ticket_publish_hook_(this);
    }
#endif

    if (generation != attach_generation() || epoch != authority_epoch() ||
        release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        !unsafe_route_active(route.generation, HidTransport::kUsb) ||
        !mounted_and_active(Interface::kMouse) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required() ||
        mouse.in_flight.load(std::memory_order_acquire) ||
        mouse.slot_state.load(std::memory_order_acquire) != kSlotEmpty) {
        const bool authority_lost = generation != attach_generation() || epoch != authority_epoch();
        mouse_ticket_.outcome.store(
            authority_lost ? MouseReportTicketOutcome::kAuthorityLost
                           : MouseReportTicketOutcome::kSafetyPending,
            std::memory_order_release);
        mouse_ticket_.state.store(MouseReportTicketState::kCanceled,
                                  std::memory_order_release);
        return authority_lost ? MouseReportBeginResult::kAuthorityLost
                              : MouseReportBeginResult::kSafetyPending;
    }
    MouseReportTicketState publishing = MouseReportTicketState::kWriting;
    if (!mouse_ticket_.state.compare_exchange_strong(
            publishing, MouseReportTicketState::kPublished,
            std::memory_order_release, std::memory_order_acquire)) {
        const MouseReportTicketOutcome canceled_outcome =
            mouse_ticket_.outcome.load(std::memory_order_acquire);
        return canceled_outcome == MouseReportTicketOutcome::kAuthorityLost
                   ? MouseReportBeginResult::kAuthorityLost
                   : MouseReportBeginResult::kSafetyPending;
    }
    return MouseReportBeginResult::kPublished;
}

MouseReportSnapshot StateMachine::mouse_report_snapshot() const {
    return MouseReportSnapshot{
        .state = mouse_ticket_.state.load(std::memory_order_acquire),
        .outcome = mouse_ticket_.outcome.load(std::memory_order_acquire),
    };
}

bool StateMachine::cancel_mouse_report() {
    MouseReportTicketState expected = MouseReportTicketState::kPublished;
    if (!mouse_ticket_.state.compare_exchange_strong(
            expected, MouseReportTicketState::kCanceled,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    mouse_ticket_.outcome.store(MouseReportTicketOutcome::kNotReady,
                                std::memory_order_release);
    return true;
}

void StateMachine::finalize_mouse_report() {
    auto state = mouse_ticket_.state.load(std::memory_order_acquire);
    while (state == MouseReportTicketState::kSubmitted ||
           state == MouseReportTicketState::kNotReady ||
           state == MouseReportTicketState::kCanceled) {
        if (mouse_ticket_.state.compare_exchange_weak(
                state, MouseReportTicketState::kFree,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
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
    const UsbGeneration queue_generation = attach_generation();
    const AuthorityEpoch queue_authority_epoch = authority_epoch();
    const hid_route::Snapshot queue_route = route_.snapshot();
    interface_state.slot_transport_generation = queue_generation;
    interface_state.slot_authority_epoch = queue_authority_epoch;
    interface_state.slot_route_generation = queue_route.generation;
    interface_state.slot_transport = HidTransport::kUsb;
    interface_state.slot_ticket_id = next_ticket_id_.fetch_add(1, std::memory_order_acq_rel);
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
        !safety_transport_active(interface)) {
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        return false;
    }
    interface_state.slot_state.store(kSlotReady, std::memory_order_release);
    return true;
}

void StateMachine::publish_release_request() {
    // Producers only publish a request.  Logical state, mailbox contents, and
    // safety decisions are owned by the TinyUSB executor task.
    release_request_generation_.store(attach_generation(), std::memory_order_release);
    release_request_authority_epoch_.store(authority_epoch(), std::memory_order_release);
    const std::uint32_t request_epoch =
        release_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    release_request_epoch_.store(request_epoch, std::memory_order_release);
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kSafetyPending);
    cancel_mouse_ticket(MouseReportTicketOutcome::kSafetyPending);
    usb_lifecycle_.mark_release_pending();
    // The active flag is the publication commit. An acquire observation of
    // true therefore sees both the complete identity and its safety barrier.
    release_requested_.store(true, std::memory_order_release);
}

void StateMachine::request_release_all() {
    publish_release_request();
    reconcile_unavailable_zero_work_release();
}

LifecycleSafetyResult StateMachine::begin_lifecycle_detach_safety() {
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    if (lifecycle.desired != usb_lifecycle::DesiredExposure::kHidden ||
        lifecycle.observed != usb_lifecycle::ObservedState::kDetaching) {
        return LifecycleSafetyResult::kUncertain;
    }

    bool requires_all_up = false;
    for (const Interface interface : {Interface::kKeyboard, Interface::kMouse}) {
        InterfaceState &interface_state = state(interface);
        const bool known_clean = known_all_up(interface) &&
                                 !interface_state.in_flight.load(std::memory_order_acquire) &&
                                 !interface_state.host_state_uncertain.load(std::memory_order_acquire);
        if (!known_clean) {
            requires_all_up = true;
            interface_state.safety_required.store(true, std::memory_order_release);
        }
    }
    if (!requires_all_up) {
        usb_lifecycle_.mark_release_confirmed();
        return LifecycleSafetyResult::kClean;
    }
    request_release_all();
    return LifecycleSafetyResult::kPending;
}

LifecycleSafetyResult StateMachine::begin_route_release_safety(
    hid_route::Snapshot stage_a) {
    const hid_route::Snapshot current = route_.snapshot();
    const usb_lifecycle::Snapshot lifecycle = usb_lifecycle_.snapshot();
    if (!current.coherent || current.invalidation_pending ||
        current.desired != hid_route::OutputRoute::kNone ||
        current.active != hid_route::OutputRoute::kUsb ||
        current.transition != hid_route::Transition::kReleasing ||
        current.generation != stage_a.generation ||
        lifecycle.desired != usb_lifecycle::DesiredExposure::kExposed ||
        lifecycle.observed != usb_lifecycle::ObservedState::kMounted) {
        return LifecycleSafetyResult::kUncertain;
    }

    bool requires_all_up = false;
    for (const Interface interface : {Interface::kKeyboard, Interface::kMouse}) {
        InterfaceState &interface_state = state(interface);
        const bool known_clean = known_all_up(interface) &&
                                 !interface_state.in_flight.load(std::memory_order_acquire) &&
                                 !interface_state.host_state_uncertain.load(std::memory_order_acquire);
        if (!known_clean) {
            requires_all_up = true;
            interface_state.safety_required.store(true, std::memory_order_release);
        }
    }
    if (!requires_all_up) {
        usb_lifecycle_.mark_release_confirmed();
        return LifecycleSafetyResult::kClean;
    }
    request_release_all();
    return LifecycleSafetyResult::kPending;
}

bool StateMachine::lifecycle_detach_safety_clean() const {
    return !any_safety_required() &&
           !interfaces_[0].in_flight.load(std::memory_order_acquire) &&
           !interfaces_[1].in_flight.load(std::memory_order_acquire) &&
           !interfaces_[0].host_state_uncertain.load(std::memory_order_acquire) &&
           !interfaces_[1].host_state_uncertain.load(std::memory_order_acquire);
}

void StateMachine::mark_lifecycle_detach_uncertain(UsbGeneration old_generation) {
    for (InterfaceState &interface_state : interfaces_) {
        interface_state.safety_required.store(true, std::memory_order_release);
        interface_state.host_state_uncertain.store(true, std::memory_order_release);
        interface_state.in_flight.store(false, std::memory_order_release);
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
    }
    usb_lifecycle_.mark_release_uncertain_for_generation(old_generation);
}

void StateMachine::on_driver_uninstalled() {
    cancel_release_ticket();
    cancel_keyboard_ticket(KeyboardReportTicketOutcome::kAuthorityLost);
    cancel_mouse_ticket(MouseReportTicketOutcome::kAuthorityLost);
    status_bits_.store(0, std::memory_order_release);
    if (!usb_lifecycle_.snapshot().host_release_uncertain) {
        clear_interface(interfaces_[0]);
        clear_interface(interfaces_[1]);
    } else {
        for (InterfaceState &interface_state : interfaces_) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            interface_state.in_flight.store(false, std::memory_order_release);
            interface_state.safety_required.store(true, std::memory_order_release);
            interface_state.host_state_uncertain.store(true, std::memory_order_release);
            interface_state.logical_state_held.store(false, std::memory_order_release);
        }
    }
    release_request_generation_.store(0, std::memory_order_release);
    release_request_authority_epoch_.store(0, std::memory_order_release);
    release_request_epoch_.store(0, std::memory_order_release);
    release_requested_.store(false, std::memory_order_release);
}

void StateMachine::complete_usb_install_success() {
    usb_lifecycle_.complete_install_success();
}

void StateMachine::complete_usb_install_clean_failure(std::int32_t error_code) {
    usb_lifecycle_.complete_install_clean_failure(error_code);
}

void StateMachine::complete_usb_install_ambiguous_failure(std::int32_t error_code) {
    usb_lifecycle_.complete_install_ambiguous_failure(error_code);
}

UsbGeneration StateMachine::begin_usb_uninstall() {
    return usb_lifecycle_.begin_uninstall();
}

void StateMachine::complete_usb_uninstall_success() {
    usb_lifecycle_.complete_uninstall_success();
}

void StateMachine::complete_usb_uninstall_failure(std::int32_t error_code) {
    usb_lifecycle_.complete_uninstall_failure(error_code);
}

void StateMachine::complete_usb_detach_route_invalidation(hid_route::Snapshot old_route) {
    (void)route_.invalidate_if_matches(old_route);
}

void StateMachine::begin_release_all() {
    // Only the UART/control task starts a public operation. A second request
    // while one is being observed coalesces with the existing mailbox work.
    if (release_ticket_.active.load(std::memory_order_acquire)) {
        return;
    }
    release_ticket_.transport_generation.store(attach_generation(), std::memory_order_release);
    release_ticket_.authority_epoch.store(authority_epoch(), std::memory_order_release);
    const hid_route::Snapshot route = route_.snapshot();
    release_ticket_.route_generation.store(route.generation, std::memory_order_release);
    release_ticket_.transport.store(HidTransport::kUsb, std::memory_order_release);
    release_ticket_.keyboard.store(ReleaseAllInterfaceState::kUnresolved,
                                    std::memory_order_release);
    release_ticket_.mouse.store(ReleaseAllInterfaceState::kUnresolved,
                                std::memory_order_release);
    release_ticket_.failed_before_finalization.store(false, std::memory_order_release);
    release_ticket_.canceled.store(false, std::memory_order_release);
    release_ticket_.finalized.store(false, std::memory_order_release);
    release_ticket_.active.store(true, std::memory_order_release);

    bool needs_safety_request = false;
    for (const Interface interface : {Interface::kKeyboard, Interface::kMouse}) {
        if (known_all_up(interface)) {
            set_release_outcome(interface, ReleaseAllInterfaceState::kAlreadyUp);
            continue;
        }
        needs_safety_request = true;
        InterfaceState &interface_state = state(interface);
        const std::uint8_t slot_state = interface_state.slot_state.load(std::memory_order_acquire);
        const bool existing_safety = interface_state.safety_required.load(std::memory_order_acquire) ||
                                     interface_state.in_flight.load(std::memory_order_acquire) ||
                                     slot_state == kSlotWriting || slot_state == kSlotReady ||
                                     slot_state == kSlotExecuting;
        if (existing_safety || !mounted_and_active(interface)) {
            if (!mounted_and_active(interface) &&
                (interface_state.logical_state_held.load(std::memory_order_acquire) ||
                 interface_state.host_state_uncertain.load(std::memory_order_acquire))) {
                // The public operation is pending because TinyUSB cannot
                // accept a report now, but the safety requirement persists so
                // the executor can perform the all-up release once readiness
                // returns. This is not a replay of the public request.
                interface_state.safety_required.store(true, std::memory_order_release);
            }
            set_release_outcome(interface, ReleaseAllInterfaceState::kPending);
        }
    }

    if (needs_safety_request) {
        request_release_all();
    }
}

ReleaseAllSnapshot StateMachine::release_all_snapshot() const {
    return ReleaseAllSnapshot{
        .transport_generation = release_ticket_.transport_generation.load(std::memory_order_acquire),
        .authority_epoch = release_ticket_.authority_epoch.load(std::memory_order_acquire),
        .route_generation = release_ticket_.route_generation.load(std::memory_order_acquire),
        .transport = release_ticket_.transport.load(std::memory_order_acquire),
        .keyboard = release_ticket_.keyboard.load(std::memory_order_acquire),
        .mouse = release_ticket_.mouse.load(std::memory_order_acquire),
        .active = release_ticket_.active.load(std::memory_order_acquire),
        .finalized = release_ticket_.finalized.load(std::memory_order_acquire),
        .failed_before_finalization =
            release_ticket_.failed_before_finalization.load(std::memory_order_acquire),
        .canceled = release_ticket_.canceled.load(std::memory_order_acquire),
    };
}

void StateMachine::finalize_release_all() {
    release_ticket_.finalized.store(true, std::memory_order_release);
    release_ticket_.active.store(false, std::memory_order_release);
}

void StateMachine::cancel_queued(Interface interface) {
    InterfaceState &interface_state = state(interface);
    std::uint8_t expected = kSlotReady;
    interface_state.slot_state.compare_exchange_strong(
        expected, kSlotCanceled, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool StateMachine::process_keyboard_ticket(SubmitFn submit, void *context,
                                            UsbGeneration current_generation,
                                            AuthorityEpoch current_authority_epoch) {
    KeyboardReportTicketState expected = KeyboardReportTicketState::kPublished;
    if (!keyboard_ticket_.state.compare_exchange_strong(
            expected, KeyboardReportTicketState::kClaimed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }

    const UsbGeneration ticket_generation =
        keyboard_ticket_.transport_generation.load(std::memory_order_relaxed);
    const HidTicketId ticket_id = keyboard_ticket_.ticket_id.load(std::memory_order_relaxed);
    const AuthorityEpoch ticket_epoch =
        keyboard_ticket_.authority_epoch.load(std::memory_order_relaxed);
    const RouteGeneration ticket_route_generation =
        keyboard_ticket_.route_generation.load(std::memory_order_relaxed);
    const HidTransport ticket_transport =
        keyboard_ticket_.transport.load(std::memory_order_relaxed);
    const std::uint32_t ticket_release_epoch =
        keyboard_ticket_.release_epoch.load(std::memory_order_relaxed);
    InterfaceState &keyboard = state(Interface::kKeyboard);
    const bool authority_lost = ticket_generation != current_generation ||
                                ticket_epoch != current_authority_epoch;
    const bool safety_pending =
        ticket_release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required();
    const bool not_ready = !mounted_and_active(Interface::kKeyboard) ||
                           !unsafe_route_active(ticket_route_generation, ticket_transport);
    const bool busy = keyboard.in_flight.load(std::memory_order_acquire) ||
                      keyboard.slot_state.load(std::memory_order_acquire) != kSlotEmpty;
    if (authority_lost || safety_pending || not_ready || busy) {
        const KeyboardReportTicketOutcome outcome =
            authority_lost       ? KeyboardReportTicketOutcome::kAuthorityLost
            : safety_pending     ? KeyboardReportTicketOutcome::kSafetyPending
            : busy               ? KeyboardReportTicketOutcome::kBusy
                                 : KeyboardReportTicketOutcome::kNotReady;
        keyboard_ticket_.outcome.store(outcome, std::memory_order_release);
        keyboard_ticket_.state.store(KeyboardReportTicketState::kCanceled,
                                     std::memory_order_release);
        return false;
    }

    // Keep the final pre-submit window explicit. Lifecycle callbacks publish
    // their epoch before control/session cleanup, so a callback observed here
    // invalidates the claimed ticket without allowing a stale key report.
#ifdef HID_RUNTIME_NATIVE_TEST
    if (before_submit_hook_ != nullptr) {
        before_submit_hook_(this);
    }
#endif
    if (ticket_generation != attach_generation() ||
        ticket_epoch != authority_epoch() ||
        ticket_release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        !unsafe_route_active(ticket_route_generation, ticket_transport) ||
        !mounted_and_active(Interface::kKeyboard) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required()) {
        keyboard_ticket_.outcome.store(
            ticket_generation != attach_generation() || ticket_epoch != authority_epoch()
                ? KeyboardReportTicketOutcome::kAuthorityLost
                : KeyboardReportTicketOutcome::kSafetyPending,
            std::memory_order_release);
        keyboard_ticket_.state.store(KeyboardReportTicketState::kCanceled,
                                     std::memory_order_release);
        return false;
    }

    const bool accepted = submit(context, static_cast<std::uint8_t>(Interface::kKeyboard),
                                 keyboard_ticket_.report, sizeof(keyboard_ticket_.report));
    if (!accepted) {
        keyboard_ticket_.outcome.store(KeyboardReportTicketOutcome::kNotReady,
                                       std::memory_order_release);
        keyboard_ticket_.state.store(KeyboardReportTicketState::kNotReady,
                                     std::memory_order_release);
        return false;
    }

    keyboard.in_flight_transport_generation = current_generation;
    keyboard.in_flight_authority_epoch = current_authority_epoch;
    keyboard.in_flight_route_generation = ticket_route_generation;
    keyboard.in_flight_transport = ticket_transport;
    keyboard.in_flight_ticket_id = ticket_id;
    keyboard.in_flight_release_epoch = ticket_release_epoch;
    keyboard.in_flight_kind = ReportKind::kUnsafeKeyboard;
    keyboard.in_flight_length = sizeof(keyboard_ticket_.report);
    std::memcpy(keyboard.in_flight_report, keyboard_ticket_.report,
                sizeof(keyboard_ticket_.report));
    keyboard.in_flight.store(true, std::memory_order_release);
    keyboard.keyboard.modifiers = keyboard_ticket_.report[0];
    for (std::size_t key_index = 0; key_index < keyboard.keyboard.keycodes.size(); ++key_index) {
        keyboard.keyboard.keycodes[key_index] = keyboard_ticket_.report[key_index + 2];
    }
    keyboard.logical_state_held.store(
        unsafe_report_holds_state(ReportKind::kUnsafeKeyboard,
                                  keyboard_ticket_.report,
                                  sizeof(keyboard_ticket_.report)),
        std::memory_order_release);
    keyboard_ticket_.outcome.store(KeyboardReportTicketOutcome::kSubmitted,
                                   std::memory_order_release);
    keyboard_ticket_.state.store(KeyboardReportTicketState::kSubmitted,
                                 std::memory_order_release);
    return true;
}

bool StateMachine::process_mouse_ticket(SubmitFn submit, void *context,
                                         UsbGeneration current_generation,
                                         AuthorityEpoch current_authority_epoch) {
    MouseReportTicketState expected = MouseReportTicketState::kPublished;
    if (!mouse_ticket_.state.compare_exchange_strong(
            expected, MouseReportTicketState::kClaimed,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }

    const UsbGeneration ticket_generation =
        mouse_ticket_.transport_generation.load(std::memory_order_relaxed);
    const HidTicketId ticket_id = mouse_ticket_.ticket_id.load(std::memory_order_relaxed);
    const AuthorityEpoch ticket_epoch =
        mouse_ticket_.authority_epoch.load(std::memory_order_relaxed);
    const RouteGeneration ticket_route_generation =
        mouse_ticket_.route_generation.load(std::memory_order_relaxed);
    const HidTransport ticket_transport =
        mouse_ticket_.transport.load(std::memory_order_relaxed);
    const std::uint32_t ticket_release_epoch =
        mouse_ticket_.release_epoch.load(std::memory_order_relaxed);
    InterfaceState &mouse = state(Interface::kMouse);
    const bool authority_lost = ticket_generation != current_generation ||
                                ticket_epoch != current_authority_epoch;
    const bool safety_pending =
        ticket_release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required();
    const bool not_ready = !mounted_and_active(Interface::kMouse) ||
                           !unsafe_route_active(ticket_route_generation, ticket_transport);
    const bool busy = mouse.in_flight.load(std::memory_order_acquire) ||
                      mouse.slot_state.load(std::memory_order_acquire) != kSlotEmpty;
    if (authority_lost || safety_pending || not_ready || busy) {
        const MouseReportTicketOutcome outcome =
            authority_lost ? MouseReportTicketOutcome::kAuthorityLost
            : safety_pending ? MouseReportTicketOutcome::kSafetyPending
            : busy ? MouseReportTicketOutcome::kBusy
                  : MouseReportTicketOutcome::kNotReady;
        mouse_ticket_.outcome.store(outcome, std::memory_order_release);
        mouse_ticket_.state.store(MouseReportTicketState::kCanceled,
                                  std::memory_order_release);
        return false;
    }

#ifdef HID_RUNTIME_NATIVE_TEST
    if (before_submit_hook_ != nullptr) {
        before_submit_hook_(this);
    }
#endif
    if (ticket_generation != attach_generation() ||
        ticket_epoch != authority_epoch() ||
        ticket_release_epoch != release_epoch_.load(std::memory_order_acquire) ||
        !unsafe_route_active(ticket_route_generation, ticket_transport) ||
        !mounted_and_active(Interface::kMouse) ||
        release_requested_.load(std::memory_order_acquire) || any_safety_required()) {
        const bool current_authority = ticket_generation == attach_generation() &&
                                       ticket_epoch == authority_epoch();
        mouse_ticket_.outcome.store(
            current_authority ? MouseReportTicketOutcome::kSafetyPending
                              : MouseReportTicketOutcome::kAuthorityLost,
            std::memory_order_release);
        mouse_ticket_.state.store(MouseReportTicketState::kCanceled,
                                  std::memory_order_release);
        return false;
    }

    const bool accepted = submit(context, static_cast<std::uint8_t>(Interface::kMouse),
                                 mouse_ticket_.report, sizeof(mouse_ticket_.report));
    if (!accepted) {
        mouse_ticket_.outcome.store(MouseReportTicketOutcome::kNotReady,
                                    std::memory_order_release);
        mouse_ticket_.state.store(MouseReportTicketState::kNotReady,
                                  std::memory_order_release);
        return false;
    }

    mouse.in_flight_transport_generation = current_generation;
    mouse.in_flight_authority_epoch = current_authority_epoch;
    mouse.in_flight_route_generation = ticket_route_generation;
    mouse.in_flight_transport = ticket_transport;
    mouse.in_flight_ticket_id = ticket_id;
    mouse.in_flight_release_epoch = ticket_release_epoch;
    mouse.in_flight_kind = ReportKind::kUnsafeMouse;
    mouse.in_flight_length = sizeof(mouse_ticket_.report);
    std::memcpy(mouse.in_flight_report, mouse_ticket_.report,
                sizeof(mouse_ticket_.report));
    mouse.in_flight.store(true, std::memory_order_release);
    mouse.mouse.buttons = mouse_ticket_.report[0] & 0x1fU;
    mouse.logical_state_held.store(
        unsafe_report_holds_state(ReportKind::kUnsafeMouse,
                                  mouse_ticket_.report,
                                  sizeof(mouse_ticket_.report)),
        std::memory_order_release);
    mouse_ticket_.outcome.store(MouseReportTicketOutcome::kSubmitted,
                                std::memory_order_release);
    mouse_ticket_.state.store(MouseReportTicketState::kSubmitted,
                              std::memory_order_release);
    return true;
}

void StateMachine::execute(SubmitFn submit, void *context) {
    if (submit == nullptr) {
        return;
    }
    const StatusSnapshot snapshot = status();
    if (!snapshot.mounted || snapshot.suspended) {
        return;
    }
    const UsbGeneration current_generation = attach_generation();
    const AuthorityEpoch current_authority_epoch = authority_epoch();
    const bool release_requested =
        release_requested_.exchange(false, std::memory_order_acq_rel);
    // Exchange first. A producer publishes its identity before setting this
    // flag, so an acquire observation of true sees one complete request. A
    // later producer leaves the flag true for the next SOF pass.
    const std::uint32_t request_generation = release_requested
        ? release_request_generation_.load(std::memory_order_acquire)
        : 0;
    const AuthorityEpoch request_authority_epoch = release_requested
        ? release_request_authority_epoch_.load(std::memory_order_acquire)
        : 0;
    const std::uint32_t request_release_epoch = release_requested
        ? release_request_epoch_.load(std::memory_order_acquire)
        : 0;
    const bool release_requested_for_current_attach =
        release_requested && request_generation == current_generation &&
        request_authority_epoch == current_authority_epoch &&
        request_release_epoch == release_epoch_.load(std::memory_order_acquire);
    // A public keyboard ticket has priority over ordinary mailboxes. It is a
    // single immediate TinyUSB call; a canceled/stale ticket never falls
    // through to a later SOF for replay.
    const bool keyboard_submitted = process_keyboard_ticket(
        submit, context, current_generation, current_authority_epoch);
    if (keyboard_submitted) {
        return;
    }
    // Mouse public work has the same immediate, task-affine semantics. Safety
    // requests cancel published mouse work before this point, and the final
    // epoch/safety checks above prevent a stale relative report.
    const bool mouse_submitted = process_mouse_ticket(
        submit, context, current_generation, current_authority_epoch);
    if (mouse_submitted) {
        return;
    }
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
        const UsbGeneration slot_transport_generation = interface_state.slot_transport_generation;
        const HidTicketId slot_ticket_id = interface_state.slot_ticket_id;
        const AuthorityEpoch slot_authority_epoch = interface_state.slot_authority_epoch;
        const RouteGeneration slot_route_generation = interface_state.slot_route_generation;
        const HidTransport slot_transport = interface_state.slot_transport;
        const std::uint32_t slot_release_epoch = interface_state.slot_release_epoch;
        const std::uint8_t length = interface_state.slot_length;
        const bool safety_kind = kind == ReportKind::kSafetyKeyboard ||
                                 kind == ReportKind::kSafetyMouse;
        const bool stale_unsafe =
            !safety_kind &&
            (slot_release_epoch != release_epoch_.load(std::memory_order_acquire) ||
             !unsafe_route_active(slot_route_generation, slot_transport));
        const bool safety_now = interface_state.safety_required.load(std::memory_order_acquire) ||
                                release_requested_.load(std::memory_order_acquire);
        const bool any_safety_pending = any_safety_required();
        if (slot_transport_generation != current_generation ||
            slot_authority_epoch != current_authority_epoch ||
            !(safety_kind ? safety_transport_active(interface) : mounted_and_active(interface)) ||
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
        // The hook deterministically models a lifecycle callback winning the
        // final claimed-to-submit boundary. Production builds have no hook.
#ifdef HID_RUNTIME_NATIVE_TEST
        if (before_submit_hook_ != nullptr) {
            before_submit_hook_(this);
        }
#endif
        if (slot_transport_generation != attach_generation() ||
            slot_authority_epoch != authority_epoch() ||
            (!safety_kind && !unsafe_route_active(slot_route_generation, slot_transport)) ||
            !(safety_kind ? safety_transport_active(interface) : mounted_and_active(interface)) ||
            interface_state.slot_state.load(std::memory_order_acquire) != kSlotExecuting) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            continue;
        }
        const bool accepted = submit(context, static_cast<std::uint8_t>(interface),
                                     interface_state.slot_report, length);
        if (!accepted) {
            interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
            if (safety_kind && release_ticket_.active.load(std::memory_order_acquire) &&
                release_ticket_.transport_generation.load(std::memory_order_acquire) == current_generation &&
                release_ticket_.authority_epoch.load(std::memory_order_acquire) == current_authority_epoch) {
                set_release_outcome(interface, ReleaseAllInterfaceState::kPending);
                release_ticket_.failed_before_finalization.store(true, std::memory_order_release);
            }
            // Unsafe reports are discarded. Safety reports remain required and
            // are retried only in the safe all-up direction.
            continue;
        }
        interface_state.in_flight_transport_generation = current_generation;
        interface_state.in_flight_authority_epoch = current_authority_epoch;
        interface_state.in_flight_route_generation = slot_route_generation;
        interface_state.in_flight_transport = slot_transport;
        interface_state.in_flight_ticket_id = slot_ticket_id;
        interface_state.in_flight_release_epoch = slot_release_epoch;
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
            interface_state.logical_state_held.store(
                unsafe_report_holds_state(kind, interface_state.slot_report, length),
                std::memory_order_release);
        } else if (kind == ReportKind::kUnsafeMouse) {
            interface_state.mouse.buttons = static_cast<std::uint8_t>(interface_state.slot_report[0] & 0x1fU);
            interface_state.logical_state_held.store(
                unsafe_report_holds_state(kind, interface_state.slot_report, length),
                std::memory_order_release);
        } else {
            interface_state.keyboard = {};
            interface_state.mouse = {};
            interface_state.logical_state_held.store(false, std::memory_order_release);
        }
        if (safety_kind && release_ticket_.active.load(std::memory_order_acquire) &&
            release_ticket_.transport_generation.load(std::memory_order_acquire) == current_generation &&
            release_ticket_.authority_epoch.load(std::memory_order_acquire) == current_authority_epoch) {
            const auto current_outcome = interface == Interface::kKeyboard
                                             ? release_ticket_.keyboard.load(std::memory_order_acquire)
                                             : release_ticket_.mouse.load(std::memory_order_acquire);
            if (current_outcome == ReleaseAllInterfaceState::kUnresolved) {
                set_release_outcome(interface, ReleaseAllInterfaceState::kSubmitted);
            }
        }
        interface_state.slot_state.store(kSlotEmpty, std::memory_order_release);
        break;
    }
    if (release_requested_for_current_attach) {
        reconcile_zero_work_release(current_generation, current_authority_epoch,
                                    request_release_epoch, false);
    }
}

HidWorkToken StateMachine::in_flight_token(Interface interface) const {
    const InterfaceState &interface_state = state(interface);
    return HidWorkToken{
        .authority_epoch = interface_state.in_flight_authority_epoch,
        .route_generation = interface_state.in_flight_route_generation,
        .transport = interface_state.in_flight_transport,
        .transport_generation = interface_state.in_flight_transport_generation,
        .ticket_id = interface_state.in_flight_ticket_id,
        .release_epoch = interface_state.in_flight_release_epoch,
    };
}

bool StateMachine::report_complete(std::uint8_t instance,
                                   const std::uint8_t *report,
                                   std::uint16_t length) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    return report_complete_for_token(instance,
                                     in_flight_token(static_cast<Interface>(instance)),
                                     report, length);
}

bool StateMachine::report_complete_for_token(std::uint8_t instance, HidWorkToken token,
                                             const std::uint8_t *report,
                                             std::uint16_t length) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    InterfaceState &interface_state = interfaces_[instance];
    if (!interface_state.in_flight.load(std::memory_order_acquire) ||
        interface_state.in_flight_transport_generation != attach_generation() ||
        interface_state.in_flight_authority_epoch != authority_epoch() ||
        (interface_state.in_flight_kind != ReportKind::kSafetyKeyboard &&
         interface_state.in_flight_kind != ReportKind::kSafetyMouse &&
         !unsafe_route_active(interface_state.in_flight_route_generation,
                              interface_state.in_flight_transport)) ||
        interface_state.in_flight_transport_generation != token.transport_generation ||
        interface_state.in_flight_authority_epoch != token.authority_epoch ||
        interface_state.in_flight_route_generation != token.route_generation ||
        interface_state.in_flight_transport != token.transport ||
        interface_state.in_flight_ticket_id != token.ticket_id ||
        interface_state.in_flight_release_epoch != token.release_epoch) {
        return false;
    }
    if (report != nullptr && length != interface_state.in_flight_length) {
        return false;
    }
    if (report != nullptr &&
        std::memcmp(report, interface_state.in_flight_report, length) != 0) {
        return false;
    }
    const ReportKind kind = interface_state.in_flight_kind;
    const std::uint8_t completed_report[8] = {
        interface_state.in_flight_report[0], interface_state.in_flight_report[1],
        interface_state.in_flight_report[2], interface_state.in_flight_report[3],
        interface_state.in_flight_report[4], interface_state.in_flight_report[5],
        interface_state.in_flight_report[6], interface_state.in_flight_report[7],
    };
    if (kind == ReportKind::kSafetyKeyboard || kind == ReportKind::kSafetyMouse) {
        interface_state.safety_required.store(false, std::memory_order_release);
        interface_state.host_state_uncertain.store(false, std::memory_order_release);
        interface_state.keyboard = {};
        interface_state.mouse = {};
        if (kind == ReportKind::kSafetyKeyboard) {
            write_confirmed_keyboard(completed_report);
        } else {
            write_confirmed_mouse(0);
        }
    } else {
        interface_state.host_state_uncertain.store(false, std::memory_order_release);
        if (kind == ReportKind::kUnsafeKeyboard) {
            write_confirmed_keyboard(completed_report);
        } else if (kind == ReportKind::kUnsafeMouse) {
            write_confirmed_mouse(completed_report[0]);
        }
    }
    // Publish the confirmed/provisional transition before clearing the
    // in-flight bit. A producer that observes !in_flight must never see the
    // previous confirmed payload and submit a duplicate same-state report.
    interface_state.in_flight.store(false, std::memory_order_release);
    if ((kind == ReportKind::kSafetyKeyboard || kind == ReportKind::kSafetyMouse) &&
        !any_safety_required()) {
        usb_lifecycle_.mark_release_confirmed();
        apply_u7_1b_compatibility_route();
    }
    return true;
}

bool StateMachine::report_failed(std::uint8_t instance,
                                 const std::uint8_t *report,
                                 std::uint16_t length) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    return report_failed_for_token(instance,
                                   in_flight_token(static_cast<Interface>(instance)),
                                   report, length);
}

bool StateMachine::report_failed_for_token(std::uint8_t instance, HidWorkToken token,
                                           const std::uint8_t *report,
                                           std::uint16_t length) {
    if (instance > static_cast<std::uint8_t>(Interface::kMouse)) {
        return false;
    }
    InterfaceState &interface_state = interfaces_[instance];
    if (!interface_state.in_flight.load(std::memory_order_acquire) ||
        interface_state.in_flight_transport_generation != attach_generation() ||
        interface_state.in_flight_authority_epoch != authority_epoch() ||
        (interface_state.in_flight_kind != ReportKind::kSafetyKeyboard &&
         interface_state.in_flight_kind != ReportKind::kSafetyMouse &&
         !unsafe_route_active(interface_state.in_flight_route_generation,
                              interface_state.in_flight_transport)) ||
        interface_state.in_flight_transport_generation != token.transport_generation ||
        interface_state.in_flight_authority_epoch != token.authority_epoch ||
        interface_state.in_flight_route_generation != token.route_generation ||
        interface_state.in_flight_transport != token.transport ||
        interface_state.in_flight_ticket_id != token.ticket_id ||
        interface_state.in_flight_release_epoch != token.release_epoch) {
        return false;
    }
    // TinyUSB reports transferred bytes for a failed input transfer; a short
    // transfer is itself the failure evidence and cannot be payload-matched.
    if (report != nullptr && length == interface_state.in_flight_length &&
        std::memcmp(report, interface_state.in_flight_report, length) != 0) {
        return false;
    }
    interface_state.host_state_uncertain.store(true, std::memory_order_release);
    interface_state.safety_required.store(true, std::memory_order_release);
    usb_lifecycle_.mark_release_uncertain();
    if (release_ticket_.active.load(std::memory_order_acquire) &&
        release_ticket_.transport_generation.load(std::memory_order_acquire) ==
            interface_state.in_flight_transport_generation &&
        release_ticket_.authority_epoch.load(std::memory_order_acquire) ==
            interface_state.in_flight_authority_epoch &&
        (interface_state.in_flight_kind == ReportKind::kSafetyKeyboard ||
         interface_state.in_flight_kind == ReportKind::kSafetyMouse)) {
        set_release_outcome(static_cast<Interface>(instance),
                            ReleaseAllInterfaceState::kPending);
        if (!release_ticket_.finalized.load(std::memory_order_acquire)) {
            release_ticket_.failed_before_finalization.store(true, std::memory_order_release);
        }
    }
    // Keep the safety/uncertainty barrier published before another producer can
    // observe the report as no longer in flight.
    interface_state.in_flight.store(false, std::memory_order_release);
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

KeyboardReportResult Runtime::keyboard_report(
    std::uint8_t modifiers, const std::array<std::uint8_t, 6> &keycodes) {
    const KeyboardReportBeginResult begin =
        state_machine_.begin_keyboard_report(modifiers, keycodes);
    if (begin == KeyboardReportBeginResult::kAlreadySet) {
        return KeyboardReportResult{.success = true,
                                    .authority_lost = false,
                                    .state = KeyboardReportState::kAlreadySet,
                                    .failure = KeyboardReportFailure::kNone};
    }
    if (begin != KeyboardReportBeginResult::kPublished) {
        const KeyboardReportFailure failure =
            begin == KeyboardReportBeginResult::kBusy
                ? KeyboardReportFailure::kBusy
                : begin == KeyboardReportBeginResult::kSafetyPending
                      ? KeyboardReportFailure::kSafetyPending
                      : begin == KeyboardReportBeginResult::kAuthorityLost
                            ? KeyboardReportFailure::kAuthorityLost
                            : KeyboardReportFailure::kNotReady;
        return KeyboardReportResult{.success = false,
                                    .authority_lost = failure == KeyboardReportFailure::kAuthorityLost,
                                    .state = KeyboardReportState::kSubmitted,
                                    .failure = failure};
    }

    constexpr TickType_t kKeyboardReportWaitTicks = pdMS_TO_TICKS(100);
    constexpr TickType_t kKeyboardReportPollTicks = pdMS_TO_TICKS(1);
    const TickType_t wait_start = xTaskGetTickCount();
    while (true) {
        const KeyboardReportSnapshot snapshot = state_machine_.keyboard_report_snapshot();
        if (snapshot.state == KeyboardReportTicketState::kSubmitted) {
            state_machine_.finalize_keyboard_report();
            return KeyboardReportResult{.success = true,
                                        .authority_lost = false,
                                        .state = KeyboardReportState::kSubmitted,
                                        .failure = KeyboardReportFailure::kNone};
        }
        if (snapshot.state == KeyboardReportTicketState::kNotReady ||
            snapshot.state == KeyboardReportTicketState::kCanceled) {
            const KeyboardReportFailure failure =
                snapshot.outcome == KeyboardReportTicketOutcome::kAuthorityLost
                    ? KeyboardReportFailure::kAuthorityLost
                    : snapshot.outcome == KeyboardReportTicketOutcome::kSafetyPending
                          ? KeyboardReportFailure::kSafetyPending
                          : snapshot.outcome == KeyboardReportTicketOutcome::kBusy
                                ? KeyboardReportFailure::kBusy
                                : KeyboardReportFailure::kNotReady;
            state_machine_.finalize_keyboard_report();
            return KeyboardReportResult{.success = false,
                                        .authority_lost = failure == KeyboardReportFailure::kAuthorityLost,
                                        .state = KeyboardReportState::kSubmitted,
                                        .failure = failure};
        }
        if (snapshot.state == KeyboardReportTicketState::kPublished) {
            if (xTaskGetTickCount() - wait_start >= kKeyboardReportWaitTicks) {
                // HID_NOT_READY is valid only when this CAS wins. If the
                // executor claimed concurrently, keep waiting for its
                // immediate terminal outcome instead of inventing an error.
                if (state_machine_.cancel_keyboard_report()) {
                    state_machine_.finalize_keyboard_report();
                    return KeyboardReportResult{.success = false,
                                                .authority_lost = false,
                                                .state = KeyboardReportState::kSubmitted,
                                                .failure = KeyboardReportFailure::kNotReady};
                }
            } else {
                vTaskDelay(kKeyboardReportPollTicks);
            }
        } else {
            // CLAIMED is a bounded TinyUSB-task section. It is never
            // canceled by the control task; yield only to let that section
            // publish its terminal outcome.
            taskYIELD();
        }
    }
}

MouseReportResult Runtime::mouse_report(std::uint8_t buttons, std::int8_t x,
                                        std::int8_t y, std::int8_t vertical,
                                        std::int8_t horizontal) {
    const MouseReportBeginResult begin =
        state_machine_.begin_mouse_report(buttons, x, y, vertical, horizontal);
    if (begin == MouseReportBeginResult::kAlreadySet) {
        return MouseReportResult{.success = true,
                                 .authority_lost = false,
                                 .state = MouseReportState::kAlreadySet,
                                 .failure = MouseReportFailure::kNone};
    }
    if (begin != MouseReportBeginResult::kPublished) {
        const MouseReportFailure failure =
            begin == MouseReportBeginResult::kBusy
                ? MouseReportFailure::kBusy
                : begin == MouseReportBeginResult::kSafetyPending
                      ? MouseReportFailure::kSafetyPending
                      : begin == MouseReportBeginResult::kAuthorityLost
                            ? MouseReportFailure::kAuthorityLost
                            : MouseReportFailure::kNotReady;
        return MouseReportResult{.success = false,
                                 .authority_lost = failure == MouseReportFailure::kAuthorityLost,
                                 .state = MouseReportState::kSubmitted,
                                 .failure = failure};
    }

    constexpr TickType_t kMouseReportWaitTicks = pdMS_TO_TICKS(100);
    constexpr TickType_t kMouseReportPollTicks = pdMS_TO_TICKS(1);
    const TickType_t wait_start = xTaskGetTickCount();
    while (true) {
        const MouseReportSnapshot snapshot = state_machine_.mouse_report_snapshot();
        if (snapshot.state == MouseReportTicketState::kSubmitted) {
            state_machine_.finalize_mouse_report();
            return MouseReportResult{.success = true,
                                     .authority_lost = false,
                                     .state = MouseReportState::kSubmitted,
                                     .failure = MouseReportFailure::kNone};
        }
        if (snapshot.state == MouseReportTicketState::kNotReady ||
            snapshot.state == MouseReportTicketState::kCanceled) {
            const MouseReportFailure failure =
                snapshot.outcome == MouseReportTicketOutcome::kAuthorityLost
                    ? MouseReportFailure::kAuthorityLost
                    : snapshot.outcome == MouseReportTicketOutcome::kSafetyPending
                          ? MouseReportFailure::kSafetyPending
                          : snapshot.outcome == MouseReportTicketOutcome::kBusy
                                ? MouseReportFailure::kBusy
                                : MouseReportFailure::kNotReady;
            state_machine_.finalize_mouse_report();
            return MouseReportResult{.success = false,
                                     .authority_lost = failure == MouseReportFailure::kAuthorityLost,
                                     .state = MouseReportState::kSubmitted,
                                     .failure = failure};
        }
        if (snapshot.state == MouseReportTicketState::kPublished) {
            if (xTaskGetTickCount() - wait_start >= kMouseReportWaitTicks) {
                if (state_machine_.cancel_mouse_report()) {
                    state_machine_.finalize_mouse_report();
                    return MouseReportResult{.success = false,
                                             .authority_lost = false,
                                             .state = MouseReportState::kSubmitted,
                                             .failure = MouseReportFailure::kNotReady};
                }
            } else {
                vTaskDelay(kMouseReportPollTicks);
            }
        } else {
            // CLAIMED is a bounded TinyUSB-task section. It is never
            // canceled by the control task; yield only to let that section
            // publish its immediate outcome.
            taskYIELD();
        }
    }
}

void Runtime::request_release_all() { state_machine_.request_release_all(); }

LifecycleSafetyResult Runtime::run_lifecycle_detach_safety() {
    const UsbGeneration old_generation = state_machine_.attach_generation();
    const LifecycleSafetyResult start = state_machine_.begin_lifecycle_detach_safety();
    if (start != LifecycleSafetyResult::kPending) {
        if (start != LifecycleSafetyResult::kClean) {
            state_machine_.mark_lifecycle_detach_uncertain(old_generation);
        }
        return start;
    }

    constexpr TickType_t kLifecycleSafetyWaitTicks = pdMS_TO_TICKS(250);
    lifecycle_safety_waiter_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    const TickType_t wait_start = xTaskGetTickCount();
    while (!state_machine_.lifecycle_detach_safety_clean()) {
        const TickType_t elapsed = xTaskGetTickCount() - wait_start;
        if (elapsed >= kLifecycleSafetyWaitTicks) {
            break;
        }
        // report_complete/report_failed signal this task directly. Waiting
        // again after the first endpoint's completion allows keyboard and
        // mouse all-up to resolve independently without a polling sleep.
        (void)ulTaskNotifyTake(pdTRUE, kLifecycleSafetyWaitTicks - elapsed);
    }
    lifecycle_safety_waiter_.store(nullptr, std::memory_order_release);
    if (state_machine_.lifecycle_detach_safety_clean()) {
        return LifecycleSafetyResult::kClean;
    }
    state_machine_.mark_lifecycle_detach_uncertain(old_generation);
    return LifecycleSafetyResult::kUncertain;
}

LifecycleSafetyResult Runtime::run_route_release_safety(
    hid_route::Snapshot stage_a) {
    const UsbGeneration old_generation = state_machine_.attach_generation();
    const LifecycleSafetyResult start =
        state_machine_.begin_route_release_safety(stage_a);
    if (start != LifecycleSafetyResult::kPending) {
        if (start != LifecycleSafetyResult::kClean) {
            state_machine_.mark_lifecycle_detach_uncertain(old_generation);
        }
        return start;
    }

    constexpr TickType_t kRouteSafetyWaitTicks = pdMS_TO_TICKS(250);
    lifecycle_safety_waiter_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    const TickType_t wait_start = xTaskGetTickCount();
    while (!state_machine_.lifecycle_detach_safety_clean()) {
        const TickType_t elapsed = xTaskGetTickCount() - wait_start;
        if (elapsed >= kRouteSafetyWaitTicks) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, kRouteSafetyWaitTicks - elapsed);
    }
    lifecycle_safety_waiter_.store(nullptr, std::memory_order_release);
    if (state_machine_.lifecycle_detach_safety_clean()) {
        return LifecycleSafetyResult::kClean;
    }
    state_machine_.mark_lifecycle_detach_uncertain(old_generation);
    return LifecycleSafetyResult::kUncertain;
}

void Runtime::on_driver_uninstalled() { state_machine_.on_driver_uninstalled(); }

ReleaseAllResult Runtime::release_all() {
    state_machine_.begin_release_all();
    constexpr TickType_t kReleaseAllWaitTicks = pdMS_TO_TICKS(100);
    constexpr TickType_t kReleaseAllPollTicks = pdMS_TO_TICKS(1);
    const TickType_t wait_start = xTaskGetTickCount();
    while (true) {
        const ReleaseAllSnapshot snapshot = state_machine_.release_all_snapshot();
        const AuthorityEpoch current_epoch = state_machine_.authority_epoch();
        const std::uint32_t current_generation = state_machine_.attach_generation();
        if (snapshot.canceled || snapshot.authority_epoch != current_epoch ||
            snapshot.transport_generation != current_generation) {
            state_machine_.finalize_release_all();
            return ReleaseAllResult{.success = false, .authority_lost = true};
        }
        const bool keyboard_terminal = snapshot.keyboard == ReleaseAllInterfaceState::kAlreadyUp ||
                                       snapshot.keyboard == ReleaseAllInterfaceState::kSubmitted ||
                                       snapshot.keyboard == ReleaseAllInterfaceState::kPending;
        const bool mouse_terminal = snapshot.mouse == ReleaseAllInterfaceState::kAlreadyUp ||
                                    snapshot.mouse == ReleaseAllInterfaceState::kSubmitted ||
                                    snapshot.mouse == ReleaseAllInterfaceState::kPending;
        if (keyboard_terminal && mouse_terminal) {
            const bool success = !snapshot.failed_before_finalization &&
                                 snapshot.keyboard != ReleaseAllInterfaceState::kPending &&
                                 snapshot.mouse != ReleaseAllInterfaceState::kPending;
            state_machine_.finalize_release_all();
            return ReleaseAllResult{.success = success,
                                    .authority_lost = false,
                                    .keyboard = snapshot.keyboard,
                                    .mouse = snapshot.mouse};
        }
        if (xTaskGetTickCount() - wait_start >= kReleaseAllWaitTicks) {
            state_machine_.finalize_release_all();
            return ReleaseAllResult{.success = false,
                                    .authority_lost = false,
                                    .keyboard = ReleaseAllInterfaceState::kPending,
                                    .mouse = ReleaseAllInterfaceState::kPending};
        }
        vTaskDelay(kReleaseAllPollTicks);
    }
}

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

void Runtime::on_report_complete(std::uint8_t instance,
                                 const std::uint8_t *report,
                                 std::uint16_t length) {
    if (state_machine_.report_complete(instance, report, length) &&
        instance <= static_cast<std::uint8_t>(Interface::kMouse)) {
        set_result(static_cast<Interface>(instance), false);
        notify_lifecycle_safety_waiter();
    }
}

bool Runtime::on_report_failed(std::uint8_t instance,
                               const std::uint8_t *report,
                               std::uint16_t length) {
    if (state_machine_.report_failed(instance, report, length) &&
        instance <= static_cast<std::uint8_t>(Interface::kMouse)) {
        set_result(static_cast<Interface>(instance), true);
        notify_lifecycle_safety_waiter();
        return true;
    }
    return false;
}

void Runtime::notify_lifecycle_safety_waiter() {
    void *const waiter = lifecycle_safety_waiter_.load(std::memory_order_acquire);
    if (waiter != nullptr) {
        xTaskNotifyGive(static_cast<TaskHandle_t>(waiter));
    }
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
