#pragma once

#include <atomic>
#include <cstdint>

namespace ble_transport::detail {

enum class LifecycleWatchdogPurpose : std::uint8_t {
    kNone,
    kSync,
    kDisconnect,
};

// Lock-free ownership protocol for the single physical lifecycle timer.
// Transitional states retain the exact owner until timer start/stop or timeout
// publication has completed, so another purpose cannot reuse the timer handle
// while an earlier owner's physical operation is still in flight.
class LifecycleWatchdogOwnership final {
  public:
    bool try_acquire(LifecycleWatchdogPurpose purpose) {
        if (purpose == LifecycleWatchdogPurpose::kNone) {
            return false;
        }
        Phase expected = Phase::kNone;
        return phase_.compare_exchange_strong(
            expected, armed_phase(purpose), std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool release_after_arm_failure(LifecycleWatchdogPurpose purpose) {
        Phase expected = armed_phase(purpose);
        return phase_.compare_exchange_strong(
            expected, Phase::kNone, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool begin_cancel(LifecycleWatchdogPurpose purpose) {
        Phase expected = armed_phase(purpose);
        return phase_.compare_exchange_strong(
            expected, cancelling_phase(purpose), std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool complete_cancel(LifecycleWatchdogPurpose purpose) {
        Phase expected = cancelling_phase(purpose);
        return phase_.compare_exchange_strong(
            expected, Phase::kNone, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    LifecycleWatchdogPurpose begin_timeout() {
        Phase observed = phase_.load(std::memory_order_acquire);
        while (true) {
            const LifecycleWatchdogPurpose purpose = armed_purpose(observed);
            if (purpose == LifecycleWatchdogPurpose::kNone) {
                return LifecycleWatchdogPurpose::kNone;
            }
            if (phase_.compare_exchange_weak(
                    observed, firing_phase(purpose), std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return purpose;
            }
        }
    }

    bool complete_timeout(LifecycleWatchdogPurpose purpose) {
        Phase expected = firing_phase(purpose);
        return phase_.compare_exchange_strong(
            expected, Phase::kNone, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    LifecycleWatchdogPurpose active_purpose() const {
        return phase_purpose(phase_.load(std::memory_order_acquire));
    }

  private:
    enum class Phase : std::uint8_t {
        kNone,
        kSyncArmed,
        kDisconnectArmed,
        kSyncCancelling,
        kDisconnectCancelling,
        kSyncFiring,
        kDisconnectFiring,
    };

    static constexpr Phase armed_phase(LifecycleWatchdogPurpose purpose) {
        return purpose == LifecycleWatchdogPurpose::kSync
                   ? Phase::kSyncArmed
                   : purpose == LifecycleWatchdogPurpose::kDisconnect
                         ? Phase::kDisconnectArmed
                         : Phase::kNone;
    }

    static constexpr Phase cancelling_phase(LifecycleWatchdogPurpose purpose) {
        return purpose == LifecycleWatchdogPurpose::kSync
                   ? Phase::kSyncCancelling
                   : purpose == LifecycleWatchdogPurpose::kDisconnect
                         ? Phase::kDisconnectCancelling
                         : Phase::kNone;
    }

    static constexpr Phase firing_phase(LifecycleWatchdogPurpose purpose) {
        return purpose == LifecycleWatchdogPurpose::kSync
                   ? Phase::kSyncFiring
                   : purpose == LifecycleWatchdogPurpose::kDisconnect
                         ? Phase::kDisconnectFiring
                         : Phase::kNone;
    }

    static constexpr LifecycleWatchdogPurpose armed_purpose(Phase phase) {
        return phase == Phase::kSyncArmed
                   ? LifecycleWatchdogPurpose::kSync
                   : phase == Phase::kDisconnectArmed
                         ? LifecycleWatchdogPurpose::kDisconnect
                         : LifecycleWatchdogPurpose::kNone;
    }

    static constexpr LifecycleWatchdogPurpose phase_purpose(Phase phase) {
        switch (phase) {
            case Phase::kSyncArmed:
            case Phase::kSyncCancelling:
            case Phase::kSyncFiring:
                return LifecycleWatchdogPurpose::kSync;
            case Phase::kDisconnectArmed:
            case Phase::kDisconnectCancelling:
            case Phase::kDisconnectFiring:
                return LifecycleWatchdogPurpose::kDisconnect;
            case Phase::kNone:
                return LifecycleWatchdogPurpose::kNone;
        }
        return LifecycleWatchdogPurpose::kNone;
    }

    static_assert(std::atomic<Phase>::is_always_lock_free);
    std::atomic<Phase> phase_{Phase::kNone};
};

}  // namespace ble_transport::detail
