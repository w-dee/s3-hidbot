#pragma once

#include <atomic>
#include <cstdint>

namespace ble_lifecycle {

enum class StopStatus : std::uint8_t {
    kIdle,
    kRunning,
    kStopped,
    kConsumed,
    kTaskFailure,
    kHostStopFailure,
    kHostExitFailure,
    kDeinitFailure,
    kTimerFailure,
    kExpired,
    kWrongOwner,
};

// One serialized control owner starts/consumes requests. A worker may finish
// much later, including after the owner has expired the request. Identity and
// terminal state share one CAS operand; there is no state-only ABA or separate
// late result field that can overwrite a replacement request's result.
class StopTransaction final {
  public:
    using Id = std::uint64_t;
    static constexpr Id kMaxId = UINT64_MAX >> 8;

    Id begin() {
        auto observed = value_.load(std::memory_order_acquire);
        const auto phase = unpack(observed);
        if ((phase != StopStatus::kIdle && phase != StopStatus::kConsumed) ||
            next_id_ == 0 || next_id_ > kMaxId) return 0;
        const Id id = next_id_++;
        return value_.compare_exchange_strong(observed, pack(id, StopStatus::kRunning),
                    std::memory_order_acq_rel, std::memory_order_acquire) ? id : 0;
    }

    bool complete(Id id, StopStatus result) {
        if (id == 0 || id > kMaxId || result < StopStatus::kStopped ||
            result == StopStatus::kConsumed || result >= StopStatus::kExpired)
            return false;
        auto expected = pack(id, StopStatus::kRunning);
        return value_.compare_exchange_strong(expected, pack(id, result),
                   std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool expire(Id id) {
        if (id == 0 || id > kMaxId) return false;
        auto observed = value_.load(std::memory_order_acquire);
        while ((observed >> 8) == id &&
               (unpack(observed) == StopStatus::kRunning ||
                unpack(observed) == StopStatus::kStopped)) {
            if (value_.compare_exchange_weak(observed, pack(id, StopStatus::kExpired),
                        std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }

    bool consume(Id id) {
        if (id == 0 || id > kMaxId) return false;
        auto expected = pack(id, StopStatus::kStopped);
        return value_.compare_exchange_strong(expected, pack(id, StopStatus::kConsumed),
                   std::memory_order_acq_rel, std::memory_order_acquire);
    }

    StopStatus status(Id id) const {
        const auto observed = value_.load(std::memory_order_acquire);
        return id != 0 && id <= kMaxId && (observed >> 8) == id
                   ? unpack(observed) : StopStatus::kWrongOwner;
    }

    bool initialization_allowed() const {
        const auto status = unpack(value_.load(std::memory_order_acquire));
        return status == StopStatus::kIdle || status == StopStatus::kConsumed;
    }

#ifdef BLE_STOP_NATIVE_TEST
    void set_next_id_for_test(Id next) { next_id_ = next; }
#endif

  private:
    static constexpr Id pack(Id id, StopStatus status) {
        return (id << 8) | static_cast<std::uint8_t>(status);
    }
    static constexpr StopStatus unpack(Id value) {
        return static_cast<StopStatus>(value & 0xff);
    }
    std::atomic<Id> value_{0};
    Id next_id_ = 1;
};

// These calls are made in a dedicated worker, never the UART/control owner.
// In particular the pinned host_stop() can wait forever. The owner's deadline
// independently expires its exact transaction and forbids any reinitialization.
// A failed step never licenses the later reclamation steps.
template <typename Operations>
StopStatus run_stop_sequence(Operations &operations) {
    if (!operations.host_stop()) return StopStatus::kHostStopFailure;
    if (!operations.await_host_exit()) return StopStatus::kHostExitFailure;
    operations.delete_host_task();
    if (!operations.deinitialize()) return StopStatus::kDeinitFailure;
    if (!operations.retire_timers()) return StopStatus::kTimerFailure;
    return StopStatus::kStopped;
}

}  // namespace ble_lifecycle
