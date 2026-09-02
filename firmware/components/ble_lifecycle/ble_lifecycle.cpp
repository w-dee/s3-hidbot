#include "ble_lifecycle/ble_lifecycle.hpp"

namespace ble_lifecycle {
namespace {
constexpr std::uint8_t encode(ObservedState observed) {
    return static_cast<std::uint8_t>(observed) << 5;
}
}  // namespace

StateMachine::StateMachine() {
    publish(DesiredExposure::kHidden, ObservedState::kUninitialized, false, false,
            false, false);
}

void StateMachine::publish(DesiredExposure desired, ObservedState observed,
                           bool stack_ready, bool advertising, bool connected,
                           bool recovery_required) {
    std::uint8_t value = encode(observed);
    value |= desired == DesiredExposure::kExposed ? kDesiredBit : 0;
    value |= stack_ready ? kStackReadyBit : 0;
    value |= advertising ? kAdvertisingBit : 0;
    value |= connected ? kConnectedBit : 0;
    value |= recovery_required ? kRecoveryBit : 0;
    state_.store(value, std::memory_order_release);
}

void StateMachine::advance_generation() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void StateMachine::clear_error() {
    error_code_.store(0, std::memory_order_release);
    error_operation_.store(static_cast<std::uint8_t>(Operation::kRuntime),
                           std::memory_order_release);
    error_present_.store(false, std::memory_order_release);
}

void StateMachine::set_error(Operation operation, std::int32_t code) {
    error_code_.store(code, std::memory_order_release);
    error_operation_.store(static_cast<std::uint8_t>(operation),
                           std::memory_order_release);
    error_present_.store(true, std::memory_order_release);
}

TransitionOutcome StateMachine::begin_enable() {
    const Snapshot current = snapshot();
    if (current.recovery_required || current.observed == ObservedState::kFault ||
        current.observed == ObservedState::kEnabling ||
        current.observed == ObservedState::kDisabling) {
        return {};
    }
    if (current.desired == DesiredExposure::kExposed &&
        (current.observed == ObservedState::kAdvertising ||
         current.observed == ObservedState::kConnected)) {
        return {.action_result = TransitionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = current};
    }
    if (current.desired != DesiredExposure::kHidden ||
        (current.observed != ObservedState::kUninitialized &&
         current.observed != ObservedState::kIdle)) {
        return {};
    }
    advance_generation();
    clear_error();
    reset_recovery_used_.store(false, std::memory_order_release);
    publish(DesiredExposure::kExposed, ObservedState::kEnabling,
            current.stack_ready, false, false, false);
    return {.action_result = TransitionResult::kAccepted,
            .snapshot_valid = true,
            .snapshot = snapshot()};
}

TransitionOutcome StateMachine::begin_disable() {
    const Snapshot current = snapshot();
    if (current.recovery_required || current.observed == ObservedState::kFault ||
        current.observed == ObservedState::kEnabling ||
        current.observed == ObservedState::kDisabling) {
        return {};
    }
    if (current.desired == DesiredExposure::kHidden &&
        (current.observed == ObservedState::kUninitialized ||
         current.observed == ObservedState::kIdle)) {
        return {.action_result = TransitionResult::kNoOp,
                .snapshot_valid = true,
                .snapshot = current};
    }
    if (current.desired != DesiredExposure::kExposed) {
        return {};
    }
    advance_generation();
    clear_error();
    publish(DesiredExposure::kHidden, ObservedState::kDisabling,
            current.stack_ready, current.advertising, current.connected, false);
    return {.action_result = TransitionResult::kAccepted,
            .snapshot_valid = true,
            .snapshot = snapshot()};
}

bool StateMachine::complete_sync(Generation generation) {
    const Snapshot current = snapshot();
    if (generation != current.generation ||
        current.desired != DesiredExposure::kExposed ||
        current.observed != ObservedState::kEnabling) {
        return false;
    }
    publish(current.desired, current.observed, true, false, false, false);
    return true;
}

bool StateMachine::complete_advertising(Generation generation) {
    const Snapshot current = snapshot();
    if (generation != current.generation ||
        current.desired != DesiredExposure::kExposed ||
        (current.observed != ObservedState::kEnabling &&
         current.observed != ObservedState::kAdvertising)) {
        return false;
    }
    publish(current.desired, ObservedState::kAdvertising, true, true, false, false);
    return true;
}

bool StateMachine::observe_connect(Generation generation,
                                   std::uint16_t connection_handle) {
    const Snapshot current = snapshot();
    if (generation != current.generation ||
        current.desired != DesiredExposure::kExposed ||
        current.observed != ObservedState::kAdvertising) {
        return false;
    }
    connection_handle_.store(connection_handle, std::memory_order_release);
    publish(current.desired, ObservedState::kConnected, true, false, true, false);
    return true;
}

bool StateMachine::observe_disconnect(Generation generation,
                                      std::uint16_t connection_handle,
                                      bool expected) {
    const Snapshot current = snapshot();
    if (generation != current.generation ||
        connection_handle_.load(std::memory_order_acquire) != connection_handle) {
        return false;
    }
    connection_handle_.store(kNoConnection, std::memory_order_release);
    if (expected && current.desired == DesiredExposure::kHidden &&
        current.observed == ObservedState::kDisabling) {
        return true;
    }
    if (current.desired != DesiredExposure::kExposed ||
        current.observed != ObservedState::kConnected) {
        return false;
    }
    advance_generation();
    publish(current.desired, ObservedState::kEnabling, true, false, false, false);
    return true;
}

bool StateMachine::complete_disable(Generation generation) {
    const Snapshot current = snapshot();
    if (generation != current.generation ||
        current.desired != DesiredExposure::kHidden ||
        current.observed != ObservedState::kDisabling) {
        return false;
    }
    connection_handle_.store(kNoConnection, std::memory_order_release);
    publish(current.desired, ObservedState::kIdle, current.stack_ready, false, false,
            false);
    return true;
}

bool StateMachine::begin_reset_recovery(Generation generation, std::int32_t reason) {
    const Snapshot current = snapshot();
    if (generation != current.generation) {
        return false;
    }
    advance_generation();
    connection_handle_.store(kNoConnection, std::memory_order_release);
    set_error(Operation::kRuntime, reason);
    if (!reset_recovery_used_.exchange(true, std::memory_order_acq_rel)) {
        publish(current.desired,
                current.desired == DesiredExposure::kExposed
                    ? ObservedState::kEnabling
                    : ObservedState::kIdle,
                false, false, false, false);
        return true;
    }
    publish(current.desired, ObservedState::kFault, false, false, false, true);
    return false;
}

void StateMachine::complete_fault(Generation generation, Operation operation,
                                  std::int32_t error_code) {
    const Snapshot current = snapshot();
    if (generation != current.generation) {
        return;
    }
    set_error(operation, error_code);
    connection_handle_.store(kNoConnection, std::memory_order_release);
    publish(current.desired, ObservedState::kFault, current.stack_ready, false, false,
            true);
}

Snapshot StateMachine::snapshot() const {
    const std::uint8_t state = state_.load(std::memory_order_acquire);
    const bool error_present = error_present_.load(std::memory_order_acquire);
    return {
        .desired = (state & kDesiredBit) ? DesiredExposure::kExposed
                                        : DesiredExposure::kHidden,
        .observed = static_cast<ObservedState>(state >> kObservedShift),
        .generation = generation_.load(std::memory_order_acquire),
        .stack_ready = (state & kStackReadyBit) != 0,
        .advertising = (state & kAdvertisingBit) != 0,
        .connected = (state & kConnectedBit) != 0,
        .recovery_required = (state & kRecoveryBit) != 0,
        .last_error = {.present = error_present,
                       .operation = static_cast<Operation>(
                           error_operation_.load(std::memory_order_acquire)),
                       .code = error_code_.load(std::memory_order_acquire)},
    };
}

Generation StateMachine::generation() const {
    return generation_.load(std::memory_order_acquire);
}

std::uint16_t StateMachine::connection_handle() const {
    return connection_handle_.load(std::memory_order_acquire);
}

#ifdef BLE_LIFECYCLE_NATIVE_TEST
void StateMachine::set_generation_for_test(Generation generation) {
    generation_.store(generation, std::memory_order_release);
}
#endif

}  // namespace ble_lifecycle
