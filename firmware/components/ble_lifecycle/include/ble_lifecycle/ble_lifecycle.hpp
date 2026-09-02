#pragma once

#include <atomic>
#include <cstdint>

namespace ble_lifecycle {

using Generation = std::uint32_t;
constexpr std::uint16_t kNoConnection = 0xffff;

enum class DesiredExposure : std::uint8_t { kHidden, kExposed };
enum class ObservedState : std::uint8_t {
    kUninitialized,
    kEnabling,
    kIdle,
    kAdvertising,
    kConnected,
    kDisabling,
    kFault,
};
enum class Operation : std::uint8_t { kEnable, kDisable, kRuntime };
enum class TransitionResult : std::uint8_t { kAccepted, kNoOp, kBusy };

struct LastError {
    bool present = false;
    Operation operation = Operation::kRuntime;
    std::int32_t code = 0;
};

struct Snapshot {
    DesiredExposure desired = DesiredExposure::kHidden;
    ObservedState observed = ObservedState::kUninitialized;
    Generation generation = 0;
    bool stack_ready = false;
    bool advertising = false;
    bool connected = false;
    bool recovery_required = false;
    LastError last_error{};
};

struct TransitionOutcome {
    TransitionResult action_result = TransitionResult::kBusy;
    bool snapshot_valid = false;
    Snapshot snapshot{};
};

// Mutation is serialized by hid_control_executor. Atomic fields make status
// reads and callback identity capture bounded and lock-free on ESP32-S3.
class StateMachine {
  public:
    StateMachine();
    TransitionOutcome begin_enable();
    TransitionOutcome begin_disable();

    bool complete_sync(Generation generation);
    bool complete_advertising(Generation generation);
    bool observe_connect(Generation generation, std::uint16_t connection_handle);
    bool observe_disconnect(Generation generation, std::uint16_t connection_handle,
                            bool expected);
    bool complete_disable(Generation generation);
    bool begin_reset_recovery(Generation generation, std::int32_t reason);
    void complete_fault(Generation generation, Operation operation,
                        std::int32_t error_code);

    Snapshot snapshot() const;
    Generation generation() const;
    std::uint16_t connection_handle() const;

#ifdef BLE_LIFECYCLE_NATIVE_TEST
    void set_generation_for_test(Generation generation);
#endif

  private:
    static constexpr std::uint8_t kDesiredBit = 1U << 0;
    static constexpr std::uint8_t kStackReadyBit = 1U << 1;
    static constexpr std::uint8_t kAdvertisingBit = 1U << 2;
    static constexpr std::uint8_t kConnectedBit = 1U << 3;
    static constexpr std::uint8_t kRecoveryBit = 1U << 4;
    static constexpr std::uint8_t kObservedShift = 5;

    void publish(DesiredExposure desired, ObservedState observed, bool stack_ready,
                 bool advertising, bool connected, bool recovery_required);
    void advance_generation();
    void clear_error();
    void set_error(Operation operation, std::int32_t code);

    static_assert(std::atomic<Generation>::is_always_lock_free);
    std::atomic<Generation> generation_{0};
    std::atomic<std::uint16_t> connection_handle_{kNoConnection};
    std::atomic<std::int32_t> error_code_{0};
    std::atomic<std::uint8_t> error_operation_{0};
    std::atomic_bool error_present_{false};
    std::atomic<std::uint8_t> state_{0};
    std::atomic_bool reset_recovery_used_{false};
};

}  // namespace ble_lifecycle
