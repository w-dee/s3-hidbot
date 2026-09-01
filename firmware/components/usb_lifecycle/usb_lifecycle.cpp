#include "usb_lifecycle/usb_lifecycle.hpp"

namespace usb_lifecycle {
namespace {

constexpr std::uint8_t encode(ObservedState state) {
    return static_cast<std::uint8_t>(state) << 3;
}

}  // namespace

StateMachine::StateMachine() { initialize_current_boot_policy(); }

void StateMachine::initialize_current_boot_policy() {
    generation_.store(0, std::memory_order_release);
    uncertainty_generation_.store(0, std::memory_order_release);
    state_.store(kDesiredBit | encode(ObservedState::kDriverNotInstalled),
                 std::memory_order_release);
}

void StateMachine::advance_generation() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

DesiredExposure StateMachine::desired() const {
    return (state_.load(std::memory_order_acquire) & kDesiredBit) != 0
               ? DesiredExposure::kExposed
               : DesiredExposure::kHidden;
}

ObservedState StateMachine::observed() const {
    return static_cast<ObservedState>(
        state_.load(std::memory_order_acquire) >> kObservedShift);
}

void StateMachine::set_observed(ObservedState observed_state) {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        const std::uint8_t desired_state = static_cast<std::uint8_t>(
            (current & ((1U << kObservedShift) - 1U)) | encode(observed_state));
        if (state_.compare_exchange_weak(current, desired_state,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return;
        }
    } while (true);
}

void StateMachine::request_attach(Executor &executor) {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        const ObservedState previous_observed =
            static_cast<ObservedState>(current >> kObservedShift);
        if ((current & kDesiredBit) != 0 && previous_observed == ObservedState::kAttaching) {
            return;
        }
        const std::uint8_t desired_state = static_cast<std::uint8_t>(
            current | kDesiredBit);
        const std::uint8_t with_observed = static_cast<std::uint8_t>(
            (desired_state & ((1U << kObservedShift) - 1U)) | encode(ObservedState::kAttaching));
        if (state_.compare_exchange_weak(current, with_observed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            advance_generation();
            const ExecutorAction action = previous_observed == ObservedState::kDriverNotInstalled
                                              ? ExecutorAction::kInstallAndConnect
                                              : ExecutorAction::kConnect;
            executor.schedule(action, snapshot());
            return;
        }
    } while (true);
}

void StateMachine::request_detach(Executor &executor) {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        if ((current & kDesiredBit) == 0 &&
            static_cast<ObservedState>(current >> kObservedShift) == ObservedState::kDetaching) {
            return;
        }
        const std::uint8_t desired_state = static_cast<std::uint8_t>(
            (current & static_cast<std::uint8_t>(~kDesiredBit)) | kSafetyPendingBit);
        const std::uint8_t with_observed = static_cast<std::uint8_t>(
            (desired_state & ((1U << kObservedShift) - 1U)) | encode(ObservedState::kDetaching));
        if (state_.compare_exchange_weak(current, with_observed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            advance_generation();
            executor.schedule(ExecutorAction::kDisconnect, snapshot());
            return;
        }
    } while (true);
}

bool StateMachine::observe_mount() {
    if (desired() != DesiredExposure::kExposed || observed() == ObservedState::kDetaching) {
        return false;
    }
    if (observed() != ObservedState::kMounted) {
        advance_generation();
    }
    set_observed(ObservedState::kMounted);
    return true;
}

bool StateMachine::observe_unmount() {
    if (observed() != ObservedState::kDisconnected) {
        advance_generation();
    }
    set_observed(ObservedState::kDisconnected);
    return true;
}

bool StateMachine::observe_suspend() {
    if (desired() != DesiredExposure::kExposed || observed() != ObservedState::kMounted) {
        return false;
    }
    set_observed(ObservedState::kSuspended);
    return true;
}

bool StateMachine::observe_resume() {
    if (desired() != DesiredExposure::kExposed || observed() != ObservedState::kSuspended) {
        return false;
    }
    set_observed(ObservedState::kMounted);
    return true;
}

void StateMachine::mark_release_pending() {
    state_.fetch_or(kSafetyPendingBit, std::memory_order_acq_rel);
}

void StateMachine::mark_release_confirmed() {
    state_.fetch_and(static_cast<std::uint8_t>(~(kSafetyPendingBit | kUncertainBit)),
                     std::memory_order_acq_rel);
    uncertainty_generation_.store(0, std::memory_order_release);
}

void StateMachine::mark_release_uncertain() {
    mark_release_uncertain_for_generation(generation());
}

void StateMachine::mark_release_uncertain_for_generation(Generation generation) {
    state_.fetch_or(kSafetyPendingBit | kUncertainBit, std::memory_order_acq_rel);
    uncertainty_generation_.store(generation, std::memory_order_release);
}

Snapshot StateMachine::snapshot() const {
    const std::uint8_t state = state_.load(std::memory_order_acquire);
    return Snapshot{
        .desired = (state & kDesiredBit) != 0 ? DesiredExposure::kExposed : DesiredExposure::kHidden,
        .observed = static_cast<ObservedState>(state >> kObservedShift),
        .generation = generation_.load(std::memory_order_acquire),
        .uncertainty_generation = uncertainty_generation_.load(std::memory_order_acquire),
        .safety_pending = (state & kSafetyPendingBit) != 0,
        .host_release_uncertain = (state & kUncertainBit) != 0,
    };
}

Generation StateMachine::generation() const {
    return generation_.load(std::memory_order_acquire);
}

bool StateMachine::accepts_hid(bool endpoint_ready) const {
    const Snapshot current = snapshot();
    // Safety pending blocks unsafe work in hid_runtime, but it must not
    // prevent the lifecycle-bound all-up operation from executing.
    return current.desired == DesiredExposure::kExposed &&
           current.observed == ObservedState::kMounted && endpoint_ready;
}

bool StateMachine::has_unresolved_prior_generation() const {
    const Snapshot current = snapshot();
    return current.host_release_uncertain &&
           current.uncertainty_generation != current.generation;
}

#ifdef USB_LIFECYCLE_NATIVE_TEST
void StateMachine::set_generation_for_test(Generation generation) {
    generation_.store(generation, std::memory_order_release);
}
#endif

}  // namespace usb_lifecycle
