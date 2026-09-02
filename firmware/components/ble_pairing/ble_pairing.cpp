#include "ble_pairing/ble_pairing.hpp"

#include <limits>

namespace ble_pairing {
namespace {
constexpr std::uint8_t kPairingActive = 1U << 0;
constexpr std::uint8_t kInputConsumed = 1U << 1;
constexpr std::uint8_t kIdExhausted = 1U << 2;
}  // namespace

void StateMachine::publish(LiveState live_state, LastResult last_result,
                           ble_lifecycle::Generation generation,
                           std::uint16_t connection_handle,
                           std::uint32_t pairing_id, bool pairing_active,
                           bool input_consumed, bool id_exhausted) {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    generation_.store(generation, std::memory_order_relaxed);
    connection_handle_.store(connection_handle, std::memory_order_relaxed);
    pairing_id_.store(pairing_id, std::memory_order_relaxed);
    live_state_.store(static_cast<std::uint8_t>(live_state),
                      std::memory_order_relaxed);
    last_result_.store(static_cast<std::uint8_t>(last_result),
                       std::memory_order_relaxed);
    std::uint8_t flags = pairing_active ? kPairingActive : 0;
    flags |= input_consumed ? kInputConsumed : 0;
    flags |= id_exhausted ? kIdExhausted : 0;
    flags_.store(flags, std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_release);
}

bool StateMachine::current(ble_lifecycle::Generation generation,
                           std::uint16_t connection_handle) const {
    const Snapshot value = snapshot();
    return value.coherent && value.generation == generation &&
           value.connection_handle == connection_handle;
}

void StateMachine::begin_connection(ble_lifecycle::Generation generation,
                                    std::uint16_t connection_handle) {
    const Snapshot old = snapshot();
    publish(LiveState::kSecuring, old.last_result, generation,
            connection_handle, 0, false, false, old.id_exhausted);
}

PasskeyActionResult StateMachine::begin_passkey_input(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle) {
    const Snapshot old = snapshot();
    if (!old.coherent || old.generation != generation ||
        old.connection_handle != connection_handle ||
        old.live_state == LiveState::kIdle) {
        return PasskeyActionResult::kStale;
    }
    if (old.live_state == LiveState::kWaitingInput && old.pairing_id != 0) {
        return PasskeyActionResult::kDuplicate;
    }
    if (old.id_exhausted) {
        publish(LiveState::kIdle, LastResult::kSecurityPolicy, generation,
                connection_handle, 0, false, false, true);
        return PasskeyActionResult::kIdExhausted;
    }
    const std::uint32_t id = next_pairing_id_.load(std::memory_order_relaxed);
    if (id == 0) {
        publish(LiveState::kIdle, LastResult::kSecurityPolicy, generation,
                connection_handle, 0, false, false, true);
        return PasskeyActionResult::kIdExhausted;
    }
    const bool exhausted = id == std::numeric_limits<std::uint32_t>::max();
    next_pairing_id_.store(exhausted ? 0 : id + 1,
                           std::memory_order_relaxed);
    publish(LiveState::kWaitingInput, LastResult::kNone, generation,
            connection_handle, id, true, false, exhausted);
    return PasskeyActionResult::kStarted;
}

PasskeyActionResult StateMachine::reject_unsupported_action(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle) {
    if (!current(generation, connection_handle)) {
        return PasskeyActionResult::kStale;
    }
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, LastResult::kSecurityPolicy, generation,
            connection_handle, 0, false, false, old.id_exhausted);
    return PasskeyActionResult::kUnsupported;
}

RespondResult StateMachine::validate_response(
    ble_lifecycle::Generation generation, std::uint16_t connection_handle,
    std::uint32_t pairing_id) const {
    const Snapshot value = snapshot();
    if (!value.coherent || value.generation != generation) {
        return RespondResult::kStaleGeneration;
    }
    if (value.connection_handle != connection_handle) {
        return RespondResult::kStaleConnection;
    }
    if (value.live_state != LiveState::kWaitingInput ||
        !value.pairing_active || value.input_consumed || value.pairing_id == 0) {
        return RespondResult::kNotPending;
    }
    return value.pairing_id == pairing_id ? RespondResult::kAccepted
                                          : RespondResult::kStalePairingId;
}

void StateMachine::consume_response(ble_lifecycle::Generation generation,
                                    std::uint16_t connection_handle,
                                    std::uint32_t pairing_id) {
    if (validate_response(generation, connection_handle, pairing_id) !=
        RespondResult::kAccepted) {
        return;
    }
    const Snapshot old = snapshot();
    publish(LiveState::kSecuring, old.last_result, generation,
            connection_handle, 0, true, true, old.id_exhausted);
}

bool StateMachine::timeout(ble_lifecycle::Generation generation,
                           std::uint16_t connection_handle,
                           std::uint32_t pairing_id) {
    if (validate_response(generation, connection_handle, pairing_id) !=
        RespondResult::kAccepted) {
        return false;
    }
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, LastResult::kTimeout, generation,
            connection_handle, 0, false, false, old.id_exhausted);
    return true;
}

bool StateMachine::complete(ble_lifecycle::Generation generation,
                            std::uint16_t connection_handle,
                            LastResult result) {
    if (!current(generation, connection_handle)) {
        return false;
    }
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, result, generation, connection_handle, 0, false,
            false, old.id_exhausted);
    return true;
}

void StateMachine::fail_closed(ble_lifecycle::Generation generation,
                               std::uint16_t connection_handle,
                               LastResult result) {
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, result, generation, connection_handle, 0, false,
            false, old.id_exhausted);
}

bool StateMachine::disconnect(ble_lifecycle::Generation generation,
                              std::uint16_t connection_handle) {
    if (!current(generation, connection_handle)) {
        return false;
    }
    const Snapshot old = snapshot();
    const LastResult result = old.pairing_active
                                  ? LastResult::kPeerDisconnected
                                  : old.last_result;
    publish(LiveState::kIdle, result, generation,
            ble_lifecycle::kNoConnection, 0, false, false, old.id_exhausted);
    return true;
}

void StateMachine::disable() {
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, LastResult::kNone, old.generation,
            ble_lifecycle::kNoConnection, 0, false, false, old.id_exhausted);
}

void StateMachine::reset() {
    const Snapshot old = snapshot();
    publish(LiveState::kIdle, LastResult::kNone, old.generation,
            ble_lifecycle::kNoConnection, 0, false, false, old.id_exhausted);
}

Snapshot StateMachine::snapshot() const {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const std::uint32_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;
        Snapshot result{};
        result.generation = generation_.load(std::memory_order_relaxed);
        result.connection_handle =
            connection_handle_.load(std::memory_order_relaxed);
        result.pairing_id = pairing_id_.load(std::memory_order_relaxed);
        result.live_state = static_cast<LiveState>(
            live_state_.load(std::memory_order_relaxed));
        result.last_result = static_cast<LastResult>(
            last_result_.load(std::memory_order_relaxed));
        const std::uint8_t flags = flags_.load(std::memory_order_relaxed);
        result.pairing_active = (flags & kPairingActive) != 0;
        result.input_consumed = (flags & kInputConsumed) != 0;
        result.id_exhausted = (flags & kIdExhausted) != 0;
        const std::uint32_t after = sequence_.load(std::memory_order_acquire);
        if (before == after) {
            result.coherent = true;
            return result;
        }
    }
    return {};
}

#ifdef BLE_PAIRING_NATIVE_TEST
void StateMachine::set_next_pairing_id_for_test(std::uint32_t value) {
    next_pairing_id_.store(value, std::memory_order_release);
    const Snapshot old = snapshot();
    publish(old.live_state, old.last_result, old.generation,
            old.connection_handle, old.pairing_id, old.pairing_active,
            old.input_consumed, value == 0);
}
#endif

}  // namespace ble_pairing
