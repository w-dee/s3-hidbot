#include <cassert>
#include <cstdint>
#include <limits>

#include "ble_pairing/ble_pairing.hpp"

namespace {
constexpr ble_lifecycle::Generation kGeneration = 9;
constexpr std::uint16_t kHandle = 37;

void transaction_and_sticky_results() {
    ble_pairing::StateMachine state;
    state.begin_connection(kGeneration, kHandle);
    auto snapshot = state.snapshot();
    assert(snapshot.coherent);
    assert(snapshot.live_state == ble_pairing::LiveState::kSecuring);
    assert(snapshot.pairing_id == 0);

    assert(state.begin_passkey_input(kGeneration, kHandle) ==
           ble_pairing::PasskeyActionResult::kStarted);
    snapshot = state.snapshot();
    const auto first_id = snapshot.pairing_id;
    assert(first_id != 0 && snapshot.pairing_active);
    assert(snapshot.live_state == ble_pairing::LiveState::kWaitingInput);
    assert(state.begin_passkey_input(kGeneration, kHandle) ==
           ble_pairing::PasskeyActionResult::kDuplicate);
    assert(state.snapshot().pairing_id == first_id);

    assert(state.validate_response(kGeneration - 1, kHandle, first_id) ==
           ble_pairing::RespondResult::kStaleGeneration);
    assert(state.validate_response(kGeneration, kHandle + 1, first_id) ==
           ble_pairing::RespondResult::kStaleConnection);
    assert(state.validate_response(kGeneration, kHandle, first_id + 1) ==
           ble_pairing::RespondResult::kStalePairingId);
    state.consume_response(kGeneration, kHandle, first_id);
    snapshot = state.snapshot();
    assert(snapshot.live_state == ble_pairing::LiveState::kSecuring);
    assert(snapshot.pairing_id == 0 && snapshot.input_consumed);
    assert(state.validate_response(kGeneration, kHandle, first_id) ==
           ble_pairing::RespondResult::kNotPending);

    assert(state.complete(kGeneration, kHandle,
                          ble_pairing::LastResult::kSucceeded));
    assert(state.snapshot().live_state == ble_pairing::LiveState::kIdle);
    assert(state.snapshot().last_result ==
           ble_pairing::LastResult::kSucceeded);

    state.begin_connection(kGeneration + 1, kHandle + 1);
    assert(state.snapshot().last_result ==
           ble_pairing::LastResult::kSucceeded);
    assert(state.begin_passkey_input(kGeneration + 1, kHandle + 1) ==
           ble_pairing::PasskeyActionResult::kStarted);
    assert(state.snapshot().last_result == ble_pairing::LastResult::kNone);
    assert(state.snapshot().pairing_id > first_id);
    state.disable();
    assert(state.snapshot().live_state == ble_pairing::LiveState::kIdle);
    assert(state.snapshot().last_result == ble_pairing::LastResult::kNone);
}

void timeout_disconnect_and_policy() {
    ble_pairing::StateMachine state;
    state.begin_connection(kGeneration, kHandle);
    assert(state.begin_passkey_input(kGeneration, kHandle) ==
           ble_pairing::PasskeyActionResult::kStarted);
    const auto id = state.snapshot().pairing_id;
    assert(!state.timeout(kGeneration - 1, kHandle, id));
    assert(!state.timeout(kGeneration, kHandle, id + 1));
    assert(state.timeout(kGeneration, kHandle, id));
    assert(state.snapshot().live_state == ble_pairing::LiveState::kIdle);
    assert(state.snapshot().last_result == ble_pairing::LastResult::kTimeout);

    state.begin_connection(kGeneration + 1, kHandle + 1);
    assert(state.begin_passkey_input(kGeneration + 1, kHandle + 1) ==
           ble_pairing::PasskeyActionResult::kStarted);
    assert(state.disconnect(kGeneration + 1, kHandle + 1));
    assert(state.snapshot().last_result ==
           ble_pairing::LastResult::kPeerDisconnected);
    assert(state.snapshot().connection_handle == ble_lifecycle::kNoConnection);

    state.begin_connection(kGeneration + 2, kHandle + 2);
    assert(state.reject_unsupported_action(kGeneration + 2, kHandle + 2) ==
           ble_pairing::PasskeyActionResult::kUnsupported);
    assert(state.snapshot().last_result ==
           ble_pairing::LastResult::kSecurityPolicy);
}

void id_wrap_is_fail_closed() {
    ble_pairing::StateMachine state;
    state.set_next_pairing_id_for_test(
        std::numeric_limits<std::uint32_t>::max());
    state.begin_connection(kGeneration, kHandle);
    assert(state.begin_passkey_input(kGeneration, kHandle) ==
           ble_pairing::PasskeyActionResult::kStarted);
    assert(state.snapshot().pairing_id ==
           std::numeric_limits<std::uint32_t>::max());
    state.consume_response(kGeneration, kHandle, state.snapshot().pairing_id);
    assert(state.begin_passkey_input(kGeneration, kHandle) ==
           ble_pairing::PasskeyActionResult::kIdExhausted);
    assert(state.snapshot().pairing_id == 0);
    assert(state.snapshot().id_exhausted);
}
}  // namespace

int main() {
    static_assert(ble_pairing::kInputTimeoutMs == 25000);
    transaction_and_sticky_results();
    timeout_disconnect_and_policy();
    id_wrap_is_fail_closed();
    return 0;
}
