#pragma once

#include <atomic>
#include <cstdint>

#include "ble_lifecycle/ble_lifecycle.hpp"

namespace ble_pairing {

constexpr std::uint32_t kInputTimeoutMs = 25000;

enum class LiveState : std::uint8_t { kIdle, kSecuring, kWaitingInput };

enum class LastResult : std::uint8_t {
    kNone,
    kSucceeded,
    kSmpFailed,
    kTimeout,
    kPeerDisconnected,
    kStoreFull,
    kStorage,
    kQueueOverflow,
    kRepeatPairing,
    kSecurityPolicy,
};

enum class PasskeyActionResult : std::uint8_t {
    kStarted,
    kDuplicate,
    kStale,
    kUnsupported,
    kIdExhausted,
};

enum class RespondResult : std::uint8_t {
    kAccepted,
    kNotPending,
    kStaleGeneration,
    kStaleConnection,
    kStalePairingId,
    kInvalidSecret,
    kInjectionFailed,
};

struct Snapshot {
    bool coherent = false;
    LiveState live_state = LiveState::kIdle;
    LastResult last_result = LastResult::kNone;
    ble_lifecycle::Generation generation = 0;
    std::uint16_t connection_handle = ble_lifecycle::kNoConnection;
    std::uint32_t pairing_id = 0;
    bool pairing_active = false;
    bool input_consumed = false;
    bool id_exhausted = false;
};

// Mutations are executor-owned. Atomic publication keeps the public status
// snapshot fixed-size and safe for bounded cross-task observation.
class StateMachine {
  public:
    void begin_connection(ble_lifecycle::Generation generation,
                          std::uint16_t connection_handle);
    PasskeyActionResult begin_passkey_input(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle);
    PasskeyActionResult reject_unsupported_action(
        ble_lifecycle::Generation generation,
        std::uint16_t connection_handle);
    RespondResult validate_response(ble_lifecycle::Generation generation,
                                    std::uint16_t connection_handle,
                                    std::uint32_t pairing_id) const;
    void consume_response(ble_lifecycle::Generation generation,
                          std::uint16_t connection_handle,
                          std::uint32_t pairing_id);
    bool timeout(ble_lifecycle::Generation generation,
                 std::uint16_t connection_handle, std::uint32_t pairing_id);
    bool complete(ble_lifecycle::Generation generation,
                  std::uint16_t connection_handle, LastResult result);
    void fail_closed(ble_lifecycle::Generation generation,
                     std::uint16_t connection_handle, LastResult result);
    bool disconnect(ble_lifecycle::Generation generation,
                    std::uint16_t connection_handle);
    void disable();
    void reset();
    Snapshot snapshot() const;

#ifdef BLE_PAIRING_NATIVE_TEST
    void set_next_pairing_id_for_test(std::uint32_t value);
#endif

  private:
    void publish(LiveState live_state, LastResult last_result,
                 ble_lifecycle::Generation generation,
                 std::uint16_t connection_handle, std::uint32_t pairing_id,
                 bool pairing_active, bool input_consumed, bool id_exhausted);
    bool current(ble_lifecycle::Generation generation,
                 std::uint16_t connection_handle) const;

    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<ble_lifecycle::Generation> generation_{0};
    std::atomic<std::uint16_t> connection_handle_{
        ble_lifecycle::kNoConnection};
    std::atomic<std::uint32_t> pairing_id_{0};
    std::atomic<std::uint32_t> next_pairing_id_{1};
    std::atomic<std::uint8_t> live_state_{0};
    std::atomic<std::uint8_t> last_result_{0};
    std::atomic<std::uint8_t> flags_{0};
};

}  // namespace ble_pairing
