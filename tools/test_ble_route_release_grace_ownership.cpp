#include <cassert>
#include <cstddef>
#include <cstdint>

#include "ble_transport/route_release_grace_ownership.hpp"

namespace {

using Identity = hid_control_executor::BleRouteReleaseIdentity;
using Ownership = ble_transport::detail::RouteReleaseGraceOwnership;

Identity identity(std::uint32_t activation_epoch) {
    Identity result{
        .authority_epoch = 10,
        .route_generation = 20,
        .profile_activation_epoch = activation_epoch,
        .ble_generation = 30,
        .connection_handle = 7,
        .present_roles = hid_capability::kInputRoles,
        .required_input_subscriptions = hid_capability::kInputRoles,
        .report_handles = {.values = {13, 14, 0}},
        .release_epoch = 40,
    };
    return result;
}

bool same_identity(Identity left, Identity right) {
    return left.authority_epoch == right.authority_epoch &&
           left.route_generation == right.route_generation &&
           left.profile_activation_epoch == right.profile_activation_epoch &&
           left.ble_generation == right.ble_generation &&
           left.connection_handle == right.connection_handle &&
           left.present_roles == right.present_roles &&
           left.required_input_subscriptions ==
               right.required_input_subscriptions &&
           left.report_handles.values == right.report_handles.values &&
           left.release_epoch == right.release_epoch;
}

void test_expired_a_before_callback_ownership_cannot_signal_b() {
    Ownership ownership;
    const Identity a = identity(1);
    const Identity b = identity(2);  // Deliberately reuses every numeric handle.
    const auto arm_a = ownership.begin_arm(a);
    assert(arm_a.valid());

    // The one-shot is already inactive and A's callback has entered its
    // deterministic pause before asking the ownership layer for its identity.
    const auto cancel_a = ownership.begin_cancel(a);
    assert(cancel_a.valid());
    ownership.finish_cancel(cancel_a, false);
    const auto arm_b = ownership.begin_arm(b);
    assert(arm_b.valid() && arm_b.slot != arm_a.slot);

    const auto stale_a = ownership.begin_callback(arm_a.slot);
    assert(stale_a.disposition ==
           Ownership::CallbackDisposition::kSuppressed);
    const auto current_b = ownership.begin_callback(arm_b.slot);
    assert(current_b.disposition == Ownership::CallbackDisposition::kSignal);
    assert(same_identity(current_b.identity, b));
    assert(ownership.available_slots_for_test() == Ownership::kSlotCount);
}

void test_owned_a_remains_immutable_and_b_expires_normally() {
    Ownership ownership;
    const Identity a = identity(1);
    Identity b = identity(2);
    b.authority_epoch++;
    b.route_generation++;
    b.ble_generation++;
    b.release_epoch++;
    const auto arm_a = ownership.begin_arm(a);
    assert(arm_a.valid());

    // A has acquired an immutable local copy but is paused before signaling.
    const auto owned_a = ownership.begin_callback(arm_a.slot);
    assert(owned_a.disposition == Ownership::CallbackDisposition::kSignal);
    assert(same_identity(owned_a.identity, a));
    const auto arm_b = ownership.begin_arm(b);
    assert(arm_b.valid());

    // Model the production controller's exact-current-identity check. A is
    // stale after B becomes current, while B's own later expiry is accepted.
    const Identity controller_current = b;
    assert(!same_identity(owned_a.identity, controller_current));
    const auto owned_b = ownership.begin_callback(arm_b.slot);
    assert(owned_b.disposition == Ownership::CallbackDisposition::kSignal);
    assert(same_identity(owned_b.identity, controller_current));
    assert(ownership.available_slots_for_test() == Ownership::kSlotCount);
}

void test_cancel_handshakes_cover_both_callback_boundaries() {
    Ownership ownership;
    const Identity a = identity(1);
    const Identity b = identity(2);

    // A successful esp_timer_stop proves that no callback was dispatched, so
    // the slot is immediately reusable. A stale cancel cannot cancel B.
    const auto armed_a = ownership.begin_arm(a);
    const auto canceled_a = ownership.begin_cancel(a);
    assert(armed_a.valid() && canceled_a.valid());
    ownership.finish_cancel(canceled_a, true);
    const auto armed_b = ownership.begin_arm(b);
    assert(armed_b.valid());
    assert(!ownership.begin_cancel(a).valid());
    const auto expired_b = ownership.begin_callback(armed_b.slot);
    assert(expired_b.disposition == Ownership::CallbackDisposition::kSignal);
    assert(same_identity(expired_b.identity, b));

    // If the callback observes cancellation while esp_timer_stop is deciding
    // that the one-shot already expired, the explicit handshake still frees
    // the slot exactly once and emits no signal.
    const auto armed_again = ownership.begin_arm(a);
    const auto canceling = ownership.begin_cancel(a);
    assert(armed_again.valid() && canceling.valid());
    const auto callback = ownership.begin_callback(armed_again.slot);
    assert(callback.disposition ==
           Ownership::CallbackDisposition::kSuppressed);
    ownership.finish_cancel(canceling, false);
    assert(ownership.available_slots_for_test() == Ownership::kSlotCount);
}

void test_repeated_cycles_do_not_leak_slots() {
    Ownership ownership;
    for (std::uint32_t iteration = 1; iteration <= 256; ++iteration) {
        Identity current = identity(iteration);
        current.release_epoch = iteration;
        const auto armed = ownership.begin_arm(current);
        assert(armed.valid());
        if (iteration % 3 == 0) {
            const auto canceled = ownership.begin_cancel(current);
            assert(canceled.valid());
            ownership.finish_cancel(canceled, true);
        } else if (iteration % 3 == 1) {
            const auto expired = ownership.begin_callback(armed.slot);
            assert(expired.disposition ==
                   Ownership::CallbackDisposition::kSignal);
            assert(same_identity(expired.identity, current));
        } else {
            const auto canceled = ownership.begin_cancel(current);
            assert(canceled.valid());
            ownership.finish_cancel(canceled, false);
            const auto expired = ownership.begin_callback(armed.slot);
            assert(expired.disposition ==
                   Ownership::CallbackDisposition::kSuppressed);
        }
        assert(ownership.available_slots_for_test() == Ownership::kSlotCount);
    }
}

}  // namespace

int main() {
    test_expired_a_before_callback_ownership_cannot_signal_b();
    test_owned_a_remains_immutable_and_b_expires_normally();
    test_cancel_handshakes_cover_both_callback_boundaries();
    test_repeated_cycles_do_not_leak_slots();
}
