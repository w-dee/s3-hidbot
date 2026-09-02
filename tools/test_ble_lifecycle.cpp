#include <cassert>
#include <cstdint>
#include <limits>

#include "ble_lifecycle/ble_lifecycle.hpp"

using namespace ble_lifecycle;

int main() {
    StateMachine state;
    auto cold = state.snapshot();
    assert(cold.desired == DesiredExposure::kHidden);
    assert(cold.observed == ObservedState::kUninitialized);
    assert(cold.generation == 0 && !cold.stack_ready && !cold.advertising &&
           !cold.connected && !cold.recovery_required && !cold.last_error.present);
    const auto cold_disable = state.begin_disable();
    assert(cold_disable.action_result == TransitionResult::kNoOp);

    const auto enable = state.begin_enable();
    assert(enable.action_result == TransitionResult::kAccepted);
    assert(enable.snapshot.observed == ObservedState::kEnabling);
    assert(enable.snapshot.generation == 1 && !enable.snapshot.stack_ready);
    assert(state.complete_sync(1));
    assert(state.complete_advertising(1));
    assert(state.begin_enable().action_result == TransitionResult::kNoOp);
    assert(state.observe_connect(1, 7));
    assert(state.snapshot().observed == ObservedState::kConnected);
    assert(state.observe_disconnect(1, 7, false));
    assert(state.snapshot().generation == 2);
    assert(state.complete_advertising(2));

    const auto disable = state.begin_disable();
    assert(disable.action_result == TransitionResult::kAccepted);
    assert(disable.snapshot.generation == 3);
    assert(disable.snapshot.advertising);
    assert(state.complete_disable(3));
    assert(state.snapshot().observed == ObservedState::kIdle);
    assert(state.snapshot().stack_ready);
    assert(state.begin_disable().action_result == TransitionResult::kNoOp);

    const auto reenable = state.begin_enable();
    assert(reenable.snapshot.stack_ready);
    assert(state.complete_sync(4));
    assert(state.complete_advertising(4));
    assert(state.observe_connect(4, 9));
    const auto connected_disable = state.begin_disable();
    assert(connected_disable.snapshot.generation == 5);
    assert(state.observe_disconnect(5, 9, true));
    assert(state.snapshot().generation == 5);
    assert(state.complete_disable(5));

    state.set_generation_for_test(std::numeric_limits<std::uint32_t>::max());
    assert(state.begin_enable().snapshot.generation == 0);
    assert(!state.complete_sync(5));
    assert(state.complete_sync(0));
    assert(state.complete_advertising(0));
    assert(!state.observe_disconnect(5, 9, false));
    assert(!state.complete_advertising(5));

    assert(state.begin_reset_recovery(0, 23));
    const auto recovery_generation = state.generation();
    assert(!state.begin_reset_recovery(0, 24));
    assert(!state.begin_reset_recovery(recovery_generation, 24));
    const auto fault = state.snapshot();
    assert(fault.observed == ObservedState::kFault && fault.recovery_required);
    assert(fault.last_error.present && fault.last_error.operation == Operation::kRuntime);
}
