#include "usb_lifecycle/usb_lifecycle.hpp"

namespace usb_lifecycle {
namespace {

constexpr std::uint8_t encode(ObservedState state) {
    return static_cast<std::uint8_t>(state) << 4;
}

}  // namespace

StateMachine::StateMachine() { initialize_hidden_boot_policy(); }

void StateMachine::initialize_hidden_boot_policy() {
    generation_.store(0, std::memory_order_release);
    uncertainty_generation_.store(0, std::memory_order_release);
    teardown_boundary_started_.store(false, std::memory_order_release);
    runtime_fault_pending_.store(false, std::memory_order_release);
    clear_last_error();
    state_.store(encode(ObservedState::kDriverNotInstalled), std::memory_order_release);
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

void StateMachine::set_recovery_required(bool required) {
    if (required) {
        state_.fetch_or(kRecoveryRequiredBit, std::memory_order_acq_rel);
    } else {
        state_.fetch_and(static_cast<std::uint8_t>(~kRecoveryRequiredBit),
                         std::memory_order_acq_rel);
    }
}

void StateMachine::clear_last_error() {
    last_error_code_.store(0, std::memory_order_release);
    last_error_operation_.store(static_cast<std::uint8_t>(LifecycleOperation::kInstall),
                                std::memory_order_release);
    last_error_present_.store(false, std::memory_order_release);
}

void StateMachine::record_error(LifecycleOperation operation, std::int32_t error_code) {
    last_error_code_.store(error_code, std::memory_order_release);
    last_error_operation_.store(static_cast<std::uint8_t>(operation),
                                std::memory_order_release);
    last_error_present_.store(true, std::memory_order_release);
}

TransitionOutcome StateMachine::request_attach(Executor &executor) {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        const ObservedState previous_observed =
            static_cast<ObservedState>(current >> kObservedShift);
        const bool exposed = (current & kDesiredBit) != 0;
        const bool recovery_required = (current & kRecoveryRequiredBit) != 0;

        if (previous_observed == ObservedState::kDetaching || recovery_required) {
            return {};
        }
        if (exposed && previous_observed != ObservedState::kDriverNotInstalled) {
            return TransitionOutcome{
                .action_result = TransitionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = snapshot(),
            };
        }
        if (!exposed && previous_observed != ObservedState::kDriverNotInstalled) {
            return {};
        }

        const std::uint8_t next = static_cast<std::uint8_t>(
            (current | kDesiredBit) & static_cast<std::uint8_t>(~kRecoveryRequiredBit));
        const std::uint8_t with_observed = static_cast<std::uint8_t>(
            (next & ((1U << kObservedShift) - 1U)) | encode(ObservedState::kAttaching));
        if (state_.compare_exchange_weak(current, with_observed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            clear_last_error();
            teardown_boundary_started_.store(false, std::memory_order_release);
            advance_generation();
            const Snapshot accepted = snapshot();
            if (executor.schedule(ExecutorAction::kInstall, accepted)) {
                return TransitionOutcome{
                    .action_result = TransitionResult::kAccepted,
                    .snapshot_valid = true,
                    .snapshot = accepted,
                };
            }
            // A queue-full result is not a recoverable normal transition:
            // retain fail-closed attaching state and make the fault visible.
            record_error(LifecycleOperation::kInstall, -1);
            set_recovery_required(true);
            return {};
        }
    } while (true);
}

TransitionOutcome StateMachine::request_detach(Executor &executor) {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        const ObservedState previous_observed =
            static_cast<ObservedState>(current >> kObservedShift);
        const bool exposed = (current & kDesiredBit) != 0;
        if (previous_observed == ObservedState::kAttaching) {
            return {};
        }
        if (!exposed && (previous_observed == ObservedState::kDriverNotInstalled ||
                         previous_observed == ObservedState::kDetaching)) {
            return TransitionOutcome{
                .action_result = TransitionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = snapshot(),
            };
        }
        if (!exposed || previous_observed == ObservedState::kDriverNotInstalled ||
            (current & kRecoveryRequiredBit) != 0) {
            return {};
        }

        const std::uint8_t next = static_cast<std::uint8_t>(
            (current & static_cast<std::uint8_t>(~kDesiredBit)) | kSafetyPendingBit);
        const std::uint8_t with_observed = static_cast<std::uint8_t>(
            (next & ((1U << kObservedShift) - 1U)) | encode(ObservedState::kDetaching));
        if (state_.compare_exchange_weak(current, with_observed,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            clear_last_error();
            // Stage A deliberately retains the current USB generation for
            // lifecycle-owned all-up safety work.
            const Snapshot accepted = snapshot();
            if (executor.schedule(ExecutorAction::kUninstall, accepted)) {
                return TransitionOutcome{
                    .action_result = TransitionResult::kAccepted,
                    .snapshot_valid = true,
                    .snapshot = accepted,
                };
            }
            record_error(LifecycleOperation::kUninstall, -1);
            set_recovery_required(true);
            return {};
        }
    } while (true);
}

void StateMachine::complete_install_success() {
    if (desired() == DesiredExposure::kExposed &&
        observed() == ObservedState::kAttaching) {
        set_recovery_required(false);
        set_observed(ObservedState::kDisconnected);
    }
}

void StateMachine::complete_install_clean_failure(std::int32_t error_code) {
    if (desired() == DesiredExposure::kExposed &&
        observed() == ObservedState::kAttaching) {
        record_error(LifecycleOperation::kInstall, error_code);
        set_recovery_required(false);
        set_observed(ObservedState::kDriverNotInstalled);
    }
}

void StateMachine::complete_install_ambiguous_failure(std::int32_t error_code) {
    if (desired() == DesiredExposure::kExposed &&
        observed() == ObservedState::kAttaching) {
        record_error(LifecycleOperation::kInstall, error_code);
        set_recovery_required(true);
    }
}

Generation StateMachine::begin_uninstall() {
    // The lifecycle executor calls this exactly once after old-generation
    // safety resolves and immediately before tinyusb_driver_uninstall().
    if (desired() == DesiredExposure::kHidden &&
        observed() == ObservedState::kDetaching &&
        !teardown_boundary_started_.exchange(true, std::memory_order_acq_rel)) {
        advance_generation();
    }
    return generation();
}

void StateMachine::complete_uninstall_success() {
    if (desired() == DesiredExposure::kHidden &&
        observed() == ObservedState::kDetaching) {
        set_recovery_required(false);
        set_observed(ObservedState::kDriverNotInstalled);
    }
}

void StateMachine::complete_uninstall_failure(std::int32_t error_code) {
    if (desired() == DesiredExposure::kHidden &&
        observed() == ObservedState::kDetaching) {
        record_error(LifecycleOperation::kUninstall, error_code);
        set_recovery_required(true);
    }
}

bool StateMachine::begin_runtime_fault(std::int32_t error_code) {
    bool expected = false;
    if (!runtime_fault_pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    // Publish the immutable primary cause before Stage A becomes observable.
    record_error(LifecycleOperation::kRuntime, error_code);
    teardown_boundary_started_.store(false, std::memory_order_release);
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        const std::uint8_t next = static_cast<std::uint8_t>(
            (current & static_cast<std::uint8_t>(~kDesiredBit)) |
            kSafetyPendingBit);
        const std::uint8_t detaching = static_cast<std::uint8_t>(
            (next & ((1U << kObservedShift) - 1U)) |
            encode(ObservedState::kDetaching));
        if (state_.compare_exchange_weak(current, detaching,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return true;
        }
    } while (true);
}

void StateMachine::update_runtime_fault(std::int32_t error_code) {
    if (runtime_fault_pending_.load(std::memory_order_acquire)) {
        record_error(LifecycleOperation::kRuntime, error_code);
    }
}

void StateMachine::complete_runtime_fault(bool driver_uninstalled) {
    if (!runtime_fault_pending_.load(std::memory_order_acquire)) {
        return;
    }
    set_recovery_required(true);
    if (driver_uninstalled) {
        set_observed(ObservedState::kDriverNotInstalled);
    }
}

bool StateMachine::runtime_fault_pending() const {
    return runtime_fault_pending_.load(std::memory_order_acquire);
}

bool StateMachine::observe_mount() {
    if (desired() != DesiredExposure::kExposed ||
        observed() == ObservedState::kDetaching ||
        snapshot().recovery_required) {
        return false;
    }
    set_observed(ObservedState::kMounted);
    return true;
}

bool StateMachine::observe_unmount() {
    if (desired() != DesiredExposure::kExposed ||
        observed() == ObservedState::kDetaching ||
        snapshot().recovery_required) {
        return false;
    }
    if (observed() != ObservedState::kDisconnected) {
        advance_generation();
    }
    set_observed(ObservedState::kDisconnected);
    return true;
}

bool StateMachine::observe_suspend() {
    if (desired() != DesiredExposure::kExposed || observed() != ObservedState::kMounted ||
        snapshot().recovery_required) {
        return false;
    }
    set_observed(ObservedState::kSuspended);
    return true;
}

bool StateMachine::observe_resume() {
    if (desired() != DesiredExposure::kExposed || observed() != ObservedState::kSuspended ||
        snapshot().recovery_required) {
        return false;
    }
    set_observed(ObservedState::kMounted);
    return true;
}

void StateMachine::mark_release_pending() {
    state_.fetch_or(kSafetyPendingBit, std::memory_order_acq_rel);
}

bool StateMachine::clear_release_pending_if_not_uncertain() {
    std::uint8_t current = state_.load(std::memory_order_acquire);
    do {
        if ((current & kUncertainBit) != 0) {
            return false;
        }
        const std::uint8_t cleared = static_cast<std::uint8_t>(
            current & static_cast<std::uint8_t>(~kSafetyPendingBit));
        if (state_.compare_exchange_weak(current, cleared,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            return true;
        }
    } while (true);
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
    const bool error_present = last_error_present_.load(std::memory_order_acquire);
    return Snapshot{
        .desired = (state & kDesiredBit) != 0 ? DesiredExposure::kExposed : DesiredExposure::kHidden,
        .observed = static_cast<ObservedState>(state >> kObservedShift),
        .generation = generation_.load(std::memory_order_acquire),
        .uncertainty_generation = uncertainty_generation_.load(std::memory_order_acquire),
        .safety_pending = (state & kSafetyPendingBit) != 0,
        .host_release_uncertain = (state & kUncertainBit) != 0,
        .recovery_required = (state & kRecoveryRequiredBit) != 0,
        .last_error = LastError{
            .present = error_present,
            .operation = static_cast<LifecycleOperation>(
                last_error_operation_.load(std::memory_order_acquire)),
            .code = last_error_code_.load(std::memory_order_acquire),
        },
    };
}

Generation StateMachine::generation() const {
    return generation_.load(std::memory_order_acquire);
}

bool StateMachine::accepts_hid(bool endpoint_ready) const {
    const Snapshot current = snapshot();
    return !current.recovery_required &&
           current.desired == DesiredExposure::kExposed &&
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
