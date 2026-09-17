#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hid_control_executor/hid_control_executor.hpp"

namespace ble_transport::detail {

// ESP_TIMER_TASK callbacks are serialized, but a one-shot becomes inactive
// before its callback enters user code. Two fixed slots let a later retirement
// arm its own timer while an expired callback is paused. A slot whose callback
// might already have been dispatched is not reusable until that callback has
// acknowledged the cancellation. Each timer's callback argument identifies
// one permanent slot, so an old callback can never acquire a later arm's slot.
class RouteReleaseGraceOwnership {
  public:
    static constexpr std::size_t kSlotCount = 2;

    struct ArmClaim {
        std::size_t slot = kSlotCount;
        std::uint64_t ownership = 0;

        constexpr bool valid() const { return slot < kSlotCount; }
    };

    using CancelClaim = ArmClaim;

    enum class CallbackDisposition : std::uint8_t {
        kIgnore,
        kSuppressed,
        kSignal,
    };

    struct CallbackClaim {
        CallbackDisposition disposition = CallbackDisposition::kIgnore;
        hid_control_executor::BleRouteReleaseIdentity identity{};
    };

    ArmClaim begin_arm(
        hid_control_executor::BleRouteReleaseIdentity identity) {
        const std::size_t first =
            next_slot_.fetch_add(1, std::memory_order_relaxed) % kSlotCount;
        for (std::size_t offset = 0; offset < kSlotCount; ++offset) {
            const std::size_t index = (first + offset) % kSlotCount;
            Slot &slot = slots_[index];
            std::uint64_t observed = slot.ownership.load(std::memory_order_acquire);
            if (state_of(observed) != SlotState::kIdle) {
                continue;
            }
            const std::uint64_t incarnation = allocate_incarnation();
            if (incarnation == 0) {
                return {};
            }
            const std::uint64_t arming = pack(incarnation, SlotState::kArming);
            if (!slot.ownership.compare_exchange_strong(
                    observed, arming, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;
            }
            slot.identity.store(identity);
            const std::uint64_t armed = pack(incarnation, SlotState::kArmed);
            slot.ownership.store(armed, std::memory_order_release);
            return {.slot = index, .ownership = armed};
        }
        return {};
    }

    void abort_arm(ArmClaim claim) {
        if (!claim.valid()) {
            return;
        }
        Slot &slot = slots_[claim.slot];
        std::uint64_t expected = claim.ownership;
        (void)slot.ownership.compare_exchange_strong(
            expected, with_state(claim.ownership, SlotState::kIdle),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    CancelClaim begin_cancel(
        hid_control_executor::BleRouteReleaseIdentity identity) {
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            Slot &slot = slots_[index];
            std::uint64_t observed = slot.ownership.load(std::memory_order_acquire);
            if (state_of(observed) != SlotState::kArmed ||
                !identities_equal(slot.identity.load(), identity)) {
                continue;
            }
            const std::uint64_t canceling =
                with_state(observed, SlotState::kCanceling);
            if (slot.ownership.compare_exchange_strong(
                    observed, canceling, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return {.slot = index, .ownership = canceling};
            }
        }
        return {};
    }

    // stopped_before_dispatch must be true only when esp_timer_stop() returned
    // ESP_OK. An inactive expired one-shot may already have a callback pending;
    // that slot stays quarantined until the pending callback observes it.
    void finish_cancel(CancelClaim claim, bool stopped_before_dispatch) {
        if (!claim.valid()) {
            return;
        }
        Slot &slot = slots_[claim.slot];
        const std::uint64_t idle =
            with_state(claim.ownership, SlotState::kIdle);
        std::uint64_t expected = claim.ownership;
        if (stopped_before_dispatch) {
            (void)slot.ownership.compare_exchange_strong(
                expected, idle, std::memory_order_acq_rel,
                std::memory_order_acquire);
            return;
        }
        const std::uint64_t awaiting = with_state(
            claim.ownership, SlotState::kCanceledAwaitingCallback);
        if (slot.ownership.compare_exchange_strong(
                expected, awaiting, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
        expected = with_state(claim.ownership,
                              SlotState::kCanceledCallbackObserved);
        (void)slot.ownership.compare_exchange_strong(
            expected, idle, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    CallbackClaim begin_callback(std::size_t index) {
        if (index >= kSlotCount) {
            return {};
        }
        Slot &slot = slots_[index];
        for (;;) {
            std::uint64_t observed =
                slot.ownership.load(std::memory_order_acquire);
            switch (state_of(observed)) {
                case SlotState::kArmed: {
                    const std::uint64_t owned =
                        with_state(observed, SlotState::kCallbackOwned);
                    if (!slot.ownership.compare_exchange_weak(
                            observed, owned, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        continue;
                    }
                    const auto identity = slot.identity.load();
                    // The callback owns an immutable local identity from here.
                    // It never accesses this slot after publishing kIdle.
                    slot.ownership.store(
                        with_state(owned, SlotState::kIdle),
                        std::memory_order_release);
                    return {.disposition = CallbackDisposition::kSignal,
                            .identity = identity};
                }
                case SlotState::kCanceling: {
                    const std::uint64_t observed_callback = with_state(
                        observed, SlotState::kCanceledCallbackObserved);
                    if (!slot.ownership.compare_exchange_weak(
                            observed, observed_callback,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        continue;
                    }
                    return {.disposition =
                                CallbackDisposition::kSuppressed};
                }
                case SlotState::kCanceledAwaitingCallback: {
                    const std::uint64_t idle =
                        with_state(observed, SlotState::kIdle);
                    if (!slot.ownership.compare_exchange_weak(
                            observed, idle, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        continue;
                    }
                    return {.disposition =
                                CallbackDisposition::kSuppressed};
                }
                default:
                    return {};
            }
        }
    }

    std::size_t available_slots_for_test() const {
        std::size_t available = 0;
        for (const Slot &slot : slots_) {
            if (state_of(slot.ownership.load(std::memory_order_acquire)) ==
                SlotState::kIdle) {
                ++available;
            }
        }
        return available;
    }

  private:
    enum class SlotState : std::uint8_t {
        kIdle,
        kArming,
        kArmed,
        kCanceling,
        kCanceledAwaitingCallback,
        kCanceledCallbackObserved,
        kCallbackOwned,
    };

    static constexpr std::uint64_t kStateMask = 0xffU;
    static constexpr unsigned kStateBits = 8;
    static constexpr std::uint64_t kMaxIncarnation =
        UINT64_MAX >> kStateBits;

    static constexpr std::uint64_t pack(std::uint64_t incarnation,
                                        SlotState state) {
        return (incarnation << kStateBits) |
               static_cast<std::uint8_t>(state);
    }

    static constexpr SlotState state_of(std::uint64_t ownership) {
        return static_cast<SlotState>(ownership & kStateMask);
    }

    static constexpr std::uint64_t with_state(std::uint64_t ownership,
                                              SlotState state) {
        return (ownership & ~kStateMask) | static_cast<std::uint8_t>(state);
    }

    std::uint64_t allocate_incarnation() {
        std::uint64_t candidate =
            next_incarnation_.load(std::memory_order_acquire);
        while (candidate != 0 && candidate <= kMaxIncarnation) {
            const std::uint64_t successor =
                candidate == kMaxIncarnation ? 0 : candidate + 1;
            if (next_incarnation_.compare_exchange_weak(
                    candidate, successor, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return candidate;
            }
        }
        return 0;
    }

    static bool identities_equal(
        hid_control_executor::BleRouteReleaseIdentity left,
        hid_control_executor::BleRouteReleaseIdentity right) {
        return left.authority_epoch == right.authority_epoch &&
               left.route_generation == right.route_generation &&
               left.profile_activation_epoch ==
                   right.profile_activation_epoch &&
               left.ble_generation == right.ble_generation &&
               left.connection_handle == right.connection_handle &&
               left.present_roles == right.present_roles &&
               left.required_input_subscriptions ==
                   right.required_input_subscriptions &&
               left.report_handles.values == right.report_handles.values &&
               left.release_epoch == right.release_epoch;
    }

    struct AtomicIdentity {
        std::atomic<hid_runtime::AuthorityEpoch> authority_epoch{0};
        std::atomic<hid_runtime::RouteGeneration> route_generation{0};
        std::atomic<hid_runtime::ProfileActivationEpoch>
            profile_activation_epoch{0};
        std::atomic<ble_lifecycle::Generation> ble_generation{0};
        std::atomic<std::uint16_t> connection_handle{
            ble_lifecycle::kNoConnection};
        std::atomic<hid_runtime::ReportMask> present_roles{0};
        std::atomic<hid_runtime::ReportMask> required_subscriptions{0};
        std::array<std::atomic<std::uint16_t>,
                   hid_capability::kReportRoleCount> report_handles{};
        std::atomic<std::uint32_t> release_epoch{0};

        void store(hid_control_executor::BleRouteReleaseIdentity identity) {
            authority_epoch.store(identity.authority_epoch,
                                  std::memory_order_relaxed);
            route_generation.store(identity.route_generation,
                                   std::memory_order_relaxed);
            profile_activation_epoch.store(identity.profile_activation_epoch,
                                           std::memory_order_relaxed);
            ble_generation.store(identity.ble_generation,
                                 std::memory_order_relaxed);
            connection_handle.store(identity.connection_handle,
                                    std::memory_order_relaxed);
            present_roles.store(identity.present_roles,
                                std::memory_order_relaxed);
            required_subscriptions.store(
                identity.required_input_subscriptions,
                std::memory_order_relaxed);
            for (std::size_t index = 0; index < report_handles.size(); ++index) {
                report_handles[index].store(
                    identity.report_handles.values[index],
                    std::memory_order_relaxed);
            }
            release_epoch.store(identity.release_epoch,
                                std::memory_order_relaxed);
        }

        hid_control_executor::BleRouteReleaseIdentity load() const {
            hid_control_executor::BleRouteReleaseIdentity identity{
                .authority_epoch =
                    authority_epoch.load(std::memory_order_relaxed),
                .route_generation =
                    route_generation.load(std::memory_order_relaxed),
                .profile_activation_epoch =
                    profile_activation_epoch.load(std::memory_order_relaxed),
                .ble_generation =
                    ble_generation.load(std::memory_order_relaxed),
                .connection_handle =
                    connection_handle.load(std::memory_order_relaxed),
                .present_roles = present_roles.load(std::memory_order_relaxed),
                .required_input_subscriptions =
                    required_subscriptions.load(std::memory_order_relaxed),
                .release_epoch =
                    release_epoch.load(std::memory_order_relaxed),
            };
            for (std::size_t index = 0; index < report_handles.size(); ++index) {
                identity.report_handles.values[index] =
                    report_handles[index].load(std::memory_order_relaxed);
            }
            return identity;
        }
    };

    struct Slot {
        std::atomic<std::uint64_t> ownership{
            pack(0, SlotState::kIdle)};
        AtomicIdentity identity{};
    };

    std::array<Slot, kSlotCount> slots_{};
    std::atomic<std::uint64_t> next_incarnation_{1};
    std::atomic<std::size_t> next_slot_{0};
};

}  // namespace ble_transport::detail
