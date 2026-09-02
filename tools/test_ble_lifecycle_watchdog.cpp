#include <cassert>

#include "ble_transport/lifecycle_watchdog.hpp"

namespace {

using ble_transport::detail::LifecycleWatchdogOwnership;
using ble_transport::detail::LifecycleWatchdogPurpose;

constexpr auto kNone = LifecycleWatchdogPurpose::kNone;
constexpr auto kSync = LifecycleWatchdogPurpose::kSync;
constexpr auto kDisconnect = LifecycleWatchdogPurpose::kDisconnect;

void test_cross_purpose_arm_is_rejected() {
    LifecycleWatchdogOwnership watchdog;
    assert(watchdog.try_acquire(kSync));
    assert(!watchdog.try_acquire(kDisconnect));
    assert(!watchdog.release_after_arm_failure(kDisconnect));
    assert(watchdog.active_purpose() == kSync);
    assert(watchdog.begin_cancel(kSync));
    assert(watchdog.active_purpose() == kSync);
    assert(watchdog.complete_cancel(kSync));

    assert(watchdog.try_acquire(kDisconnect));
    assert(!watchdog.try_acquire(kSync));
    assert(!watchdog.begin_cancel(kSync));
    assert(watchdog.active_purpose() == kDisconnect);
}

void test_same_purpose_rearm_is_rejected() {
    LifecycleWatchdogOwnership watchdog;
    assert(watchdog.try_acquire(kSync));
    assert(!watchdog.try_acquire(kSync));
    assert(watchdog.active_purpose() == kSync);
}

void test_arm_failure_releases_exact_owner() {
    LifecycleWatchdogOwnership watchdog;
    assert(watchdog.try_acquire(kDisconnect));
    assert(watchdog.release_after_arm_failure(kDisconnect));
    assert(watchdog.active_purpose() == kNone);
    assert(watchdog.try_acquire(kSync));
}

void test_cancel_and_timeout_claim_are_exclusive() {
    LifecycleWatchdogOwnership cancelled;
    assert(cancelled.try_acquire(kSync));
    assert(cancelled.begin_cancel(kSync));
    assert(cancelled.begin_timeout() == kNone);
    assert(!cancelled.try_acquire(kDisconnect));
    assert(cancelled.complete_cancel(kSync));

    LifecycleWatchdogOwnership fired;
    assert(fired.try_acquire(kDisconnect));
    assert(fired.begin_timeout() == kDisconnect);
    assert(!fired.begin_cancel(kDisconnect));
    assert(!fired.try_acquire(kSync));
    assert(fired.active_purpose() == kDisconnect);
    assert(fired.complete_timeout(kDisconnect));
    assert(fired.active_purpose() == kNone);
}

}  // namespace

int main() {
    test_cross_purpose_arm_is_rejected();
    test_same_purpose_rearm_is_rejected();
    test_arm_failure_releases_exact_owner();
    test_cancel_and_timeout_claim_are_exclusive();
    return 0;
}
